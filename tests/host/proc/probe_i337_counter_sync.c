/*
 * I-337 host model for boot-time cross-CPU clock validation.
 *
 * A reference CPU brackets each remote CPU's counter read with two local
 * reads. For one observation (ref_before, remote, ref_after), the remote
 * clock's offset from the reference lies in:
 *
 *   [remote - ref_after, remote - ref_before]
 *
 * assuming both counters advance at the same rate during the short sample.
 * Keep all intervals within +/- tolerance, and require each CPU's samples to
 * be strictly monotonic. Repeating samples over a longer window also catches
 * drift. The model uses signed offsets and does not implement counter unwrap;
 * production must use a 64-bit non-wrapping counter or unwrap safely.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#define MAX_SAMPLES 8
#define MAX_CPUS 8

struct bracket {
    uint64_t ref_before;
    uint64_t remote;
    uint64_t ref_after;
};

struct cpu_window {
    struct bracket sample[MAX_SAMPLES];
    unsigned n;
};

static int counter_pair_acceptable(const struct bracket *samples, unsigned n,
                                   int64_t tolerance)
{
    int64_t min_offset = INT64_MAX;
    int64_t max_offset = INT64_MIN;

    if (n == 0 || n > MAX_SAMPLES || tolerance < 0)
        return 0;

    for (unsigned i = 0; i < n; i++) {
        const struct bracket *s = &samples[i];
        int64_t low, high;

        /* Reject malformed, backward, or ambiguous samples before subtraction. */
        if (s->ref_after < s->ref_before || s->remote > INT64_MAX ||
            s->ref_before > INT64_MAX || s->ref_after > INT64_MAX)
            return 0;
        if (i && (s->ref_before <= samples[i - 1].ref_before ||
                  s->ref_after <= samples[i - 1].ref_after ||
                  s->remote <= samples[i - 1].remote))
            return 0;

        low = (int64_t)s->remote - (int64_t)s->ref_after;
        high = (int64_t)s->remote - (int64_t)s->ref_before;
        if (low < min_offset)
            min_offset = low;
        if (high > max_offset)
            max_offset = high;
    }

    /* This bounds both fixed offset and drift over the measurement window. */
    return min_offset >= -tolerance && max_offset <= tolerance;
}

/* CPU 0 is the reference. Each other online CPU must pass its own set of
 * bracketed reads; one bad CPU rejects the shared-clock mode for the machine. */
static int online_cpus_acceptable(const struct cpu_window *windows,
                                  unsigned ncpu, int64_t tolerance)
{
    if (ncpu == 0 || ncpu > MAX_CPUS || tolerance < 0)
        return 0;
    for (unsigned cpu = 1; cpu < ncpu; cpu++)
        if (!counter_pair_acceptable(windows[cpu].sample, windows[cpu].n,
                                     tolerance))
            return 0;
    return 1;
}

static void synchronized_case(void)
{
    const struct bracket s[] = {
        { .ref_before = 1000, .remote = 1001, .ref_after = 1002 },
        { .ref_before = 1100, .remote = 1100, .ref_after = 1101 },
        { .ref_before = 1200, .remote = 1201, .ref_after = 1202 },
    };
    assert(counter_pair_acceptable(s, sizeof(s) / sizeof(s[0]), 4));
}

static void reject_fixed_skew_case(void)
{
    const struct bracket s[] = {
        { .ref_before = 1000, .remote = 1020, .ref_after = 1002 },
        { .ref_before = 1100, .remote = 1120, .ref_after = 1102 },
    };
    assert(!counter_pair_acceptable(s, sizeof(s) / sizeof(s[0]), 4));
}

static void reject_drift_case(void)
{
    const struct bracket s[] = {
        { .ref_before = 1000, .remote = 1000, .ref_after = 1001 },
        { .ref_before = 1100, .remote = 1108, .ref_after = 1101 },
    };
    assert(!counter_pair_acceptable(s, sizeof(s) / sizeof(s[0]), 4));
}

static void reject_nonmonotonic_case(void)
{
    const struct bracket s[] = {
        { .ref_before = 1000, .remote = 1000, .ref_after = 1001 },
        { .ref_before = 1100, .remote = 999, .ref_after = 1101 },
    };
    assert(!counter_pair_acceptable(s, sizeof(s) / sizeof(s[0]), 4));
}

static void all_online_cpus_case(void)
{
    const struct cpu_window windows[] = {
        { .n = 0 }, /* CPU 0 is the reference. */
        { .n = 2, .sample = {
            { .ref_before = 1000, .remote = 1001, .ref_after = 1002 },
            { .ref_before = 2000, .remote = 2002, .ref_after = 2003 },
        }},
        { .n = 2, .sample = {
            { .ref_before = 1000, .remote = 999, .ref_after = 1001 },
            { .ref_before = 2000, .remote = 2001, .ref_after = 2002 },
        }},
    };
    assert(online_cpus_acceptable(windows, 3, 4));

    /* A single AP with fixed skew must reject shared-clock mode, even though
     * every other CPU passes. */
    struct cpu_window one_bad_cpu[3];
    for (unsigned cpu = 0; cpu < 3; cpu++)
        one_bad_cpu[cpu] = windows[cpu];
    one_bad_cpu[2].sample[0].remote += 20;
    one_bad_cpu[2].sample[1].remote += 20;
    assert(!online_cpus_acceptable(one_bad_cpu, 3, 4));
}

static void reject_wide_bracket_case(void)
{
    /* A slow/contended cross-CPU sample makes the offset interval too wide;
     * do not call that a synchronized counter just because its midpoint fits. */
    const struct bracket s[] = {
        { .ref_before = 1000, .remote = 1004, .ref_after = 1020 },
    };
    assert(!counter_pair_acceptable(s, sizeof(s) / sizeof(s[0]), 4));
}

static void reject_reference_rollback_case(void)
{
    const struct bracket s[] = {
        { .ref_before = 1000, .remote = 1000, .ref_after = 1001 },
        { .ref_before = 1100, .remote = 1101, .ref_after = 1001 },
    };
    assert(!counter_pair_acceptable(s, sizeof(s) / sizeof(s[0]), 4));
}

static void accept_tolerance_boundary_case(void)
{
    const struct bracket s[] = {
        { .ref_before = 1000, .remote = 1004, .ref_after = 1004 },
        { .ref_before = 1100, .remote = 1096, .ref_after = 1096 },
    };
    assert(counter_pair_acceptable(s, sizeof(s) / sizeof(s[0]), 4));
}

int main(void)
{
    synchronized_case();
    reject_fixed_skew_case();
    reject_drift_case();
    reject_nonmonotonic_case();
    all_online_cpus_case();
    reject_wide_bracket_case();
    reject_reference_rollback_case();
    accept_tolerance_boundary_case();
    puts("PASS: bracketed counter check validates all APs and rejects skew, drift, rollback, and wide samples");
    return 0;
}
