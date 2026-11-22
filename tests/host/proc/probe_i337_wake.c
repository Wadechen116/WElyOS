/*
 * I-337 host model: compare CPU0-only sleeper scanning with scanning on any CPU.
 *
 * This is an algorithm model, not a test of the architecture timer drivers. It
 * makes the clock assumption explicit: an all-CPU scanner is bounded only if
 * every CPU reads the same monotonically advancing real-time tick source.
 * The final case models the current timer_ticks_real() contract: global ticks
 * advance on CPU0 only, all CPUs extrapolate from CPU0's published deadline,
 * and that catch-up is capped at 100 ticks. This handles a bounded CPU0 pause,
 * but an AP cannot advance time beyond the cap while CPU0 remains stopped.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#define MAX_CPUS 8
#define REAL_CATCHUP_MAX 100u

struct sleeper {
    unsigned id;
    uint64_t deadline;
    unsigned wake_count;
    int queued;
};

static void scan_due(struct sleeper *s, unsigned n, uint64_t now)
{
    /* sched_lk serializes this loop in the kernel; one model step is atomic. */
    for (unsigned i = 0; i < n; i++) {
        if (s[i].queued && s[i].deadline <= now) {
            s[i].queued = 0;
            s[i].wake_count++;
        }
    }
}

static unsigned pending(const struct sleeper *s, unsigned n)
{
    unsigned count = 0;
    for (unsigned i = 0; i < n; i++)
        count += (unsigned)s[i].queued;
    return count;
}

static void assert_woken_once(const struct sleeper *s, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        assert(s[i].wake_count == 1 && !s[i].queued);
}

/* All CPUs see the same actual_now. Vary scan order to model timer handlers
 * arriving in different orders; sched_lk makes the dequeue exactly once. */
static void ideal_any_cpu_case(unsigned ncpu, int reverse)
{
    struct sleeper s[] = {
        { .id = 0, .deadline = 4, .queued = 1 },
        { .id = 1, .deadline = 4, .queued = 1 },
        { .id = 2, .deadline = 7, .queued = 1 },
        { .id = 3, .deadline = 9, .queued = 1 },
    };
    const unsigned ns = sizeof(s) / sizeof(s[0]);

    for (uint64_t now = 0; now <= 9; now++) {
        for (unsigned step = 0; step < ncpu; step++) {
            unsigned cpu = reverse ? ncpu - 1 - step : step;
            (void)cpu; /* The shared-clock scan operation is identical per CPU. */
            scan_due(s, ns, now);
        }
        if (now < 4)
            assert(pending(s, ns) == ns);
        if (now == 4)
            assert(s[0].wake_count == 1 && s[1].wake_count == 1 && pending(s, ns) == 2);
        if (now == 7)
            assert(s[2].wake_count == 1 && pending(s, ns) == 1);
    }
    assert_woken_once(s, ns);
}

/* CPU0-only scanner latency grows with the CPU0 pause and is unbounded if it
 * never resumes. The reference all-CPU model wakes at the actual deadline. */
static void cpu0_pause_latency_cases(void)
{
    static const unsigned pauses[] = { 0, 1, 2, 8, 32, 100, 101, 256 };
    const uint64_t deadline = 5;

    for (unsigned i = 0; i < sizeof(pauses) / sizeof(pauses[0]); i++) {
        unsigned pause = pauses[i];
        struct sleeper cpu0_only = { .id = 0, .deadline = deadline, .queued = 1 };
        struct sleeper any_cpu = { .id = 0, .deadline = deadline, .queued = 1 };
        uint64_t actual_end = deadline + pause;
        uint64_t cpu0_wake = UINT64_MAX, any_wake = UINT64_MAX;

        for (uint64_t actual = 0; actual <= actual_end; actual++) {
            /* CPU0 is unavailable from actual tick 0 through `pause - 1`. */
            if (actual >= pause) {
                scan_due(&cpu0_only, 1, actual);
                if (!cpu0_only.queued && cpu0_wake == UINT64_MAX)
                    cpu0_wake = actual;
            }
            /* Reference design: an AP scans against a shared real-time clock. */
            scan_due(&any_cpu, 1, actual);
            if (!any_cpu.queued && any_wake == UINT64_MAX)
                any_wake = actual;
        }
        assert(cpu0_only.wake_count == 1 && any_cpu.wake_count == 1);
        assert(cpu0_wake == (pause > deadline ? pause : deadline));
        assert(any_wake == deadline);
        printf("pause=%u ticks: CPU0-only wake at %llu, ideal any-CPU wake at %llu\n",
               pause, (unsigned long long)cpu0_wake, (unsigned long long)any_wake);
    }
}

/* Model timer_ticks_real() during a sustained CPU0 pause. Global ticks stay
 * fixed, but every CPU reads CPU0's stale deadline and extrapolates elapsed
 * ticks from the hardware counter. This wakes deadlines within the 100-tick
 * catch-up window; later deadlines stay pending until CPU0 advances global
 * time or its deadline is resynchronized. */
static void current_clock_source_case(void)
{
    const uint64_t frozen_global_ticks = 100;
    const uint64_t cpu0_next_deadline = 1;
    const uint64_t bounded_deadline = frozen_global_ticks + 5;
    const uint64_t beyond_cap_deadline = frozen_global_ticks + REAL_CATCHUP_MAX + 5;
    struct sleeper bounded = { .id = 0, .deadline = bounded_deadline, .queued = 1 };
    struct sleeper beyond = { .id = 1, .deadline = beyond_cap_deadline, .queued = 1 };

    for (uint64_t actual = 1; actual <= 256; actual++) {
        uint64_t d = cpu0_next_deadline;
        unsigned catchup = 0;
        while (actual >= d && catchup < REAL_CATCHUP_MAX) {
            d++;
            catchup++;
        }
        uint64_t timer_ticks_real = frozen_global_ticks + catchup;
        scan_due(&bounded, 1, timer_ticks_real);
        scan_due(&beyond, 1, timer_ticks_real);
        if (actual == 5)
            assert(bounded.wake_count == 1);
        assert(beyond.wake_count == 0);
    }

    assert_woken_once(&bounded, 1);
    assert(beyond.wake_count == 0 && beyond.queued);
    printf("current timer_ticks_real model: AP scan woke a +5 tick sleeper during CPU0 pause, "
           "but +%u stayed pending after 256 ticks (catch-up cap %u)\n",
           REAL_CATCHUP_MAX + 5, REAL_CATCHUP_MAX);
}

int main(void)
{
    for (unsigned ncpu = 1; ncpu <= MAX_CPUS; ncpu *= 2) {
        ideal_any_cpu_case(ncpu, 0);
        ideal_any_cpu_case(ncpu, 1);
    }
    cpu0_pause_latency_cases();
    current_clock_source_case();
    puts("PASS: shared-clock multi-CPU model wakes each sleeper exactly once; "
         "timer_ticks_real covers bounded pauses but saturates after its catch-up cap");
    return 0;
}
