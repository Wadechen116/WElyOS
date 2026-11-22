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
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#define MAX_CPUS 8
#define REAL_CATCHUP_MAX 100u
#define COUNTER_BITS 16u
#define COUNTER_MOD (1u << COUNTER_BITS)
#define COUNTER_MASK (COUNTER_MOD - 1u)
#define COUNTER_HZ 10000u
#define TICK_HZ 1000u

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

/* Options A and B use the same absolute candidate derived from a fixed epoch.
 * This differs from timer_ticks_real(), which adds a catch-up value to the
 * CPU0-owned global tick counter. Reusing that relative value after a CAS
 * advance would count the same elapsed time again. */
static uint64_t counter_ticks(uint16_t epoch, uint16_t sample)
{
    uint32_t delta = ((uint32_t)sample - epoch) & COUNTER_MASK;
    /* Test parameters are deliberately small enough that this product cannot
     * overflow; production mul/div code needs its own overflow proof. */
    return ((uint64_t)delta * TICK_HZ) / COUNTER_HZ;
}

static uint16_t counter_sample(uint16_t epoch, uint64_t elapsed_ms)
{
    uint64_t counts = elapsed_ms * (COUNTER_HZ / TICK_HZ);
    return (uint16_t)(((uint64_t)epoch + counts) & COUNTER_MASK);
}

static void note_wake_times(struct sleeper *s, unsigned n, uint64_t base,
                            uint64_t elapsed_ms, uint64_t *wake_at)
{
    for (unsigned i = 0; i < n; i++)
        if (!s[i].queued && wake_at[i] == UINT64_MAX)
            wake_at[i] = base + elapsed_ms;
}

/* Option A: every CPU reads one shared monotonic counter and converts it from
 * a fixed epoch. CPU0 stops for 300 ms; AP timer scans continue at 1 kHz. The
 * 16-bit test counter wraps once, and modular subtraction remains valid because
 * the whole sample interval (300 ms) is shorter than one wrap period. */
static void option_a_counter_case(void)
{
    const uint64_t base = 5000;
    const uint16_t epoch = (uint16_t)(COUNTER_MOD - 500u); /* wrap after 50 ms */
    const uint64_t elapsed_ms = 300;
    const uint64_t wrap_period_counts = COUNTER_MOD;
    const uint64_t elapsed_counts = elapsed_ms * (COUNTER_HZ / TICK_HZ);
    struct sleeper s[] = {
        { .id = 0, .deadline = base + 100, .queued = 1 },
        { .id = 1, .deadline = base + 200, .queued = 1 },
        { .id = 2, .deadline = base + 300, .queued = 1 },
    };
    const unsigned ns = sizeof(s) / sizeof(s[0]);
    uint64_t wake_at[3] = { UINT64_MAX, UINT64_MAX, UINT64_MAX };
    uint64_t last_cpu_now[MAX_CPUS] = { 0 };

    assert(elapsed_counts < wrap_period_counts); /* more than one wrap is ambiguous */
    assert(counter_sample(epoch, elapsed_ms) < epoch); /* this run crosses the raw counter wrap */
    for (uint64_t ms = 0; ms <= elapsed_ms; ms++) {
        uint16_t raw = counter_sample(epoch, ms);
        uint64_t now = base + counter_ticks(epoch, raw);
        assert(now == base + ms);
        for (unsigned step = 0; step < MAX_CPUS - 1; step++) {
            unsigned cpu = 1 + ((ms & 1) ? MAX_CPUS - 2 - step : step);
            assert(now >= last_cpu_now[cpu]);
            last_cpu_now[cpu] = now;
            scan_due(s, ns, now);
        }
        note_wake_times(s, ns, base, ms, wake_at);
    }

    assert_woken_once(s, ns);
    for (unsigned i = 0; i < ns; i++) {
        uint64_t due_ms = s[i].deadline - base;
        uint64_t observed_ms = wake_at[i] - base;
        assert(observed_ms >= due_ms && observed_ms <= due_ms + 1);
    }
    printf("option A: CPU0 stopped 300 ms; shared counter crossed one wrap; "
           "AP wake delays %llu/%llu/%llu ms (bound 1 ms)\n",
           (unsigned long long)(wake_at[0] - base - 100),
           (unsigned long long)(wake_at[1] - base - 200),
           (unsigned long long)(wake_at[2] - base - 300));

    /* A single raw sample cannot distinguish zero wraps from a full wrap. The
     * implementation must either guarantee samples less than one wrap apart
     * or extend the counter in a separate rollover mechanism. */
    assert((uint16_t)(((uint64_t)epoch + COUNTER_MOD) & COUNTER_MASK) == epoch);
    assert(counter_ticks(epoch, epoch) == 0);
}

/* Option B: CPUs publish absolute tick candidates using atomic max/CAS. A
 * stale contender may lose a race, but its retry must never lower time. */
static uint64_t cas_advance(_Atomic uint64_t *ticks, uint64_t candidate,
                            unsigned *successful_updates)
{
    uint64_t seen = atomic_load_explicit(ticks, memory_order_acquire);
    for (;;) {
        if (seen >= candidate)
            return seen;
        if (atomic_compare_exchange_weak_explicit(ticks, &seen, candidate,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            (*successful_updates)++;
            return candidate;
        }
        /* On CAS failure, seen is refreshed. The loop rechecks before trying
         * again, so an older CPU sample cannot overwrite a newer candidate. */
    }
}

static void option_b_cas_case(void)
{
    const uint64_t base = 9000;
    const uint16_t epoch = (uint16_t)(COUNTER_MOD - 700u); /* wrap after 70 ms */
    const uint64_t elapsed_ms = 300;
    const uint64_t elapsed_counts = elapsed_ms * (COUNTER_HZ / TICK_HZ);
    struct sleeper s[] = {
        { .id = 0, .deadline = base + 100, .queued = 1 },
        { .id = 1, .deadline = base + 200, .queued = 1 },
        { .id = 2, .deadline = base + 300, .queued = 1 },
    };
    const unsigned ns = sizeof(s) / sizeof(s[0]);
    uint64_t wake_at[3] = { UINT64_MAX, UINT64_MAX, UINT64_MAX };
    _Atomic uint64_t ticks;
    unsigned successful_updates = 0;
    uint64_t last = base;

    assert(elapsed_counts < COUNTER_MOD);
    assert(counter_sample(epoch, elapsed_ms) < epoch); /* same wrap case for option B */
    atomic_init(&ticks, base);

    /* Explicitly schedule an out-of-order race: a fresh +5 candidate wins,
     * then a CPU with an older +4 sample retries from the updated value. */
    {
        _Atomic uint64_t race_ticks;
        unsigned race_updates = 0;
        atomic_init(&race_ticks, base);
        uint64_t loaded_old = atomic_load_explicit(&race_ticks, memory_order_relaxed);
        uint64_t stale_candidate = base + 4;
        uint64_t fresh_candidate = base + 5;
        assert(cas_advance(&race_ticks, fresh_candidate, &race_updates) == fresh_candidate);
        uint64_t expected = loaded_old;
        assert(!atomic_compare_exchange_strong_explicit(&race_ticks, &expected, stale_candidate,
                                                        memory_order_acq_rel,
                                                        memory_order_acquire));
        assert(expected == fresh_candidate);
        assert(cas_advance(&race_ticks, stale_candidate, &race_updates) == fresh_candidate);
        assert(atomic_load_explicit(&race_ticks, memory_order_acquire) == fresh_candidate);
        assert(race_updates == 1);
    }

    for (uint64_t ms = 0; ms <= elapsed_ms; ms++) {
        uint16_t raw = counter_sample(epoch, ms);
        uint64_t candidate = base + counter_ticks(epoch, raw);
        for (unsigned step = 0; step < MAX_CPUS - 1; step++) {
            unsigned cpu = 1 + ((ms & 1) ? MAX_CPUS - 2 - step : step);
            (void)cpu;
            uint64_t published = cas_advance(&ticks, candidate, &successful_updates);
            assert(published >= last);
            last = published;
            scan_due(s, ns, published);
        }
        assert(atomic_load_explicit(&ticks, memory_order_acquire) == base + ms);
        note_wake_times(s, ns, base, ms, wake_at);
    }

    assert_woken_once(s, ns);
    for (unsigned i = 0; i < ns; i++) {
        uint64_t due_ms = s[i].deadline - base;
        uint64_t observed_ms = wake_at[i] - base;
        assert(observed_ms >= due_ms && observed_ms <= due_ms + 1);
    }
    /* Each of the 300 elapsed tick candidates advances once, not once per AP.
     * CAS publishes time only; RT/starvation/accounting side effects need a
     * separately serialized accounted-tick cursor (or a separate CPU0 path).
     * The separate +5/+4 race above also has exactly one successful publication. */
    assert(successful_updates == 300);
    printf("option B: CPU0 stopped 300 ms; CAS time is monotonic, "
           "APs publish %u advances and wake each sleeper once\n", successful_updates);
}

int main(void)
{
    for (unsigned ncpu = 1; ncpu <= MAX_CPUS; ncpu *= 2) {
        ideal_any_cpu_case(ncpu, 0);
        ideal_any_cpu_case(ncpu, 1);
    }
    cpu0_pause_latency_cases();
    current_clock_source_case();
    option_a_counter_case();
    option_b_cas_case();
    puts("PASS: A/B shared-counter models survive CPU0 pause and one counter wrap; "
         "CAS never moves time backward and every sleeper wakes once");
    return 0;
}
