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
 * Across samples, intersect these intervals to find offsets consistent with
 * every observation. The current kernel requires the whole common interval
 * to lie within +/- tolerance. A looser overlap-only check is retained as a
 * comparison because it can accept an interval that still permits an offset
 * outside tolerance. Samples wider than tolerance are ignored, and at least
 * half must remain usable. The model uses signed offsets and does not
 * implement counter unwrap; production must use a 64-bit non-wrapping counter
 * or unwrap safely.
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

struct offset_interval {
    int64_t low;
    int64_t high;
};

enum drift_result {
    DRIFT_ACCEPT,
    DRIFT_UNCERTAIN, /* interval overlaps the allowed region but is too wide */
    DRIFT_REJECT,    /* every possible change violates the strict bound */
};

static int sub_i64(int64_t a, int64_t b, int64_t *out)
{
    if ((b > 0 && a < INT64_MIN + b) ||
        (b < 0 && a > INT64_MAX + b))
        return 0;
    *out = a - b;
    return 1;
}

/* For independent intervals A and B, A-B is [A.low-B.high, A.high-B.low]. */
static int interval_subtract(struct offset_interval a, struct offset_interval b,
                             struct offset_interval *out)
{
    if (a.low > a.high || b.low > b.high ||
        !sub_i64(a.low, b.high, &out->low) ||
        !sub_i64(a.high, b.low, &out->high))
        return 0;
    return out->low <= out->high;
}

/* Convert a CPU's offset interval from this round's reference to CPU 0:
 * (CPUx - ref) - (CPU0 - ref) = CPUx - CPU0. */
static int normalize_to_cpu0(struct offset_interval cpu_x,
                             struct offset_interval cpu0,
                             struct offset_interval *normalized)
{
    return interval_subtract(cpu_x, cpu0, normalized);
}

/* I1/I2 must already describe the same CPU relative to the same reference. */
static enum drift_result cross_round_evaluate(struct offset_interval i1,
                                              struct offset_interval i2,
                                              int64_t dmax,
                                              struct offset_interval *change)
{
    if (dmax <= 0 || !interval_subtract(i2, i1, change))
        return DRIFT_REJECT;
    if (change->low > -dmax && change->high < dmax)
        return DRIFT_ACCEPT;
    if (change->low >= dmax || change->high <= -dmax)
        return DRIFT_REJECT;
    return DRIFT_UNCERTAIN;
}

/* Retry only inconclusive second-round samples; a proven violation is final. */
static enum drift_result retry_second_round(struct offset_interval i1,
                                            const struct offset_interval *i2,
                                            unsigned n2, int64_t dmax,
                                            unsigned *attempts_out)
{
    unsigned attempts = 0;
    enum drift_result result = DRIFT_UNCERTAIN;
    struct offset_interval change;

    if (!i2 || n2 == 0 || n2 > 4 || dmax <= 0) {
        if (attempts_out)
            *attempts_out = 0;
        return DRIFT_REJECT;
    }
    for (; attempts < n2; ++attempts) {
        result = cross_round_evaluate(i1, i2[attempts], dmax, &change);
        if (result != DRIFT_UNCERTAIN) {
            ++attempts;
            break;
        }
    }
    if (attempts_out)
        *attempts_out = attempts;
    return result;
}

static int counter_pair_intersection(const struct bracket *samples, unsigned n,
                                     int64_t tolerance,
                                     struct offset_interval *common,
                                     unsigned *used_out, unsigned *wide_out)
{
    unsigned used = 0, wide = 0;

    common->low = INT64_MIN;
    common->high = INT64_MAX;
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

        if (s->ref_after - s->ref_before > (uint64_t)tolerance) {
            wide++;
            continue;
        }
        low = (int64_t)s->remote - (int64_t)s->ref_after;
        high = (int64_t)s->remote - (int64_t)s->ref_before;
        if (low > common->low)
            common->low = low;
        if (high < common->high)
            common->high = high;
        used++;
    }

    if (used_out)
        *used_out = used;
    if (wide_out)
        *wide_out = wide;
    return used * 2 >= n && used != 0 && common->low <= common->high;
}

static int interval_overlaps_tolerance(struct offset_interval common,
                                       int64_t tolerance)
{
    return common.low <= tolerance && common.high >= -tolerance;
}

static int interval_strictly_within_tolerance(struct offset_interval common,
                                             int64_t tolerance)
{
    return common.low >= -tolerance && common.high <= tolerance;
}

/* Current kernel semantics: every offset in the common intersection is
 * within +/- tolerance. */
static int counter_pair_acceptable(const struct bracket *samples, unsigned n,
                                   int64_t tolerance)
{
    struct offset_interval common;
    if (!counter_pair_intersection(samples, n, tolerance, &common, NULL, NULL))
        return 0;
    return interval_strictly_within_tolerance(common, tolerance);
}

/* Explicit name for the strict production bound used above. */
static int counter_pair_strictly_bounded(const struct bracket *samples,
                                         unsigned n, int64_t tolerance)
{
    struct offset_interval common;
    if (!counter_pair_intersection(samples, n, tolerance, &common, NULL, NULL))
        return 0;
    return interval_strictly_within_tolerance(common, tolerance);
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
    assert(counter_pair_strictly_bounded(s, sizeof(s) / sizeof(s[0]), 4));
}

static void reject_fixed_skew_case(void)
{
    const struct bracket s[] = {
        { .ref_before = 1000, .remote = 1020, .ref_after = 1002 },
        { .ref_before = 1100, .remote = 1120, .ref_after = 1102 },
    };
    assert(!counter_pair_acceptable(s, sizeof(s) / sizeof(s[0]), 4));
}

static void accept_zero_skew_with_wide_samples_case(void)
{
    const struct bracket s[] = {
        /* Wide, but compatible with zero skew; ignored by the rule. */
        { .ref_before = 1000, .remote = 1003, .ref_after = 1012 },
        { .ref_before = 2000, .remote = 2000, .ref_after = 2002 },
        { .ref_before = 3000, .remote = 3007, .ref_after = 3015 },
        { .ref_before = 4000, .remote = 4002, .ref_after = 4004 },
    };
    struct offset_interval common;
    unsigned used, wide;
    assert(counter_pair_intersection(s, 4, 4, &common, &used, &wide));
    assert(used == 2 && wide == 2);
    assert(common.low <= 0 && common.high >= 0);
    assert(counter_pair_acceptable(s, 4, 4));
}

static void reject_disjoint_intervals_case(void)
{
    const struct bracket s[] = {
        { .ref_before = 1000, .remote = 1000, .ref_after = 1002 }, /* [-2, 0] */
        { .ref_before = 2000, .remote = 2003, .ref_after = 2002 }, /* [1, 3] */
    };
    struct offset_interval common;
    assert(!counter_pair_intersection(s, 2, 4, &common, NULL, NULL));
}

static void overlap_vs_strict_bound_case(void)
{
    const int64_t tolerance = 10;
    const struct offset_interval common = { .low = tolerance - 10,
                                            .high = tolerance + 10 };
    /* The overlap-only rule accepts this interval because it includes
     * allowed offsets, even though some consistent offsets exceed +tol. */
    assert(interval_overlaps_tolerance(common, tolerance));
    assert(!interval_strictly_within_tolerance(common, tolerance));

    /* A narrower, realizable bracket interval shows the same semantic gap. */
    const struct bracket s[] = {
        { .ref_before = 1000, .remote = 1005, .ref_after = 1002 }, /* [3, 5] */
    };
    assert(interval_overlaps_tolerance((struct offset_interval){ .low = 3,
                                                                 .high = 5 }, 4));
    assert(!counter_pair_acceptable(s, 1, 4));
    assert(!counter_pair_strictly_bounded(s, 1, 4));
}

static void drift_hidden_by_bracket_uncertainty_case(void)
{
    /* A feasible offset sequence can drift 0 -> 2 -> 4 counts while all
     * three uncertainty intervals still share offset 0. Intersection alone
     * therefore cannot rule out drift smaller than the bracket uncertainty. */
    const struct bracket s[] = {
        { .ref_before = 1000, .remote = 1000, .ref_after = 1004 }, /* [-4, 0] */
        { .ref_before = 1100, .remote = 1102, .ref_after = 1104 }, /* [-2, 2] */
        { .ref_before = 1200, .remote = 1204, .ref_after = 1204 }, /* [0, 4] */
    };
    assert(counter_pair_acceptable(s, 3, 4));
    assert(counter_pair_strictly_bounded(s, 3, 4));
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
        { .ref_before = 1000, .remote = 1004, .ref_after = 1004 }, /* [0, 4] */
        { .ref_before = 1100, .remote = 1100, .ref_after = 1104 }, /* [-4, 0] */
    };
    assert(counter_pair_strictly_bounded(s, sizeof(s) / sizeof(s[0]), 4));
}

static void cross_round_drift_cases(void)
{
    const int64_t dmax = 50; /* production proposal: timer_tick_counts() >> 3 */
    struct offset_interval change;

    /* (a) Two narrow rounds with a small possible change are acceptable. */
    assert(cross_round_evaluate((struct offset_interval){10, 12},
                                (struct offset_interval){11, 13},
                                dmax, &change) == DRIFT_ACCEPT);
    assert(change.low == -1 && change.high == 3);

    /* (b) Each round fits +/-100, but the cross-round change is definitely too large. */
    const struct offset_interval bad1 = {-100, -90};
    const struct offset_interval bad2 = {90, 100};
    assert(interval_strictly_within_tolerance(bad1, 100));
    assert(interval_strictly_within_tolerance(bad2, 100));
    assert(cross_round_evaluate(bad1, bad2, dmax, &change) == DRIFT_REJECT);
    assert(change.low == 180 && change.high == 200);

    /* (c) A wide interval straddles acceptable and unacceptable changes: retry. */
    const struct offset_interval i1 = {-40, 0};
    const struct offset_interval second[] = {{0, 40}, {0, 1}};
    unsigned attempts = 0;
    assert(interval_strictly_within_tolerance(i1, 100));
    assert(interval_strictly_within_tolerance(second[0], 100));
    assert(interval_strictly_within_tolerance(second[1], 100));
    assert(cross_round_evaluate(i1, second[0], dmax, &change) == DRIFT_UNCERTAIN);
    assert(change.low == 0 && change.high == 80);
    assert(retry_second_round(i1, second, 2, dmax, &attempts) == DRIFT_ACCEPT);
    assert(attempts == 2); /* only the inconclusive sample was retried */

    /* A proven violation is not retried, even when retry samples remain. */
    const struct offset_interval reject_then_pass[] = {
        {90, 100}, {0, 1},
    };
    assert(retry_second_round(bad1, reject_then_pass, 2, dmax, &attempts) == DRIFT_REJECT);
    assert(attempts == 1);

    /* (d) Different reference CPUs must be normalized to CPU 0 first.
     * Raw intervals appear to change by about 80 counts; common-reference
     * intervals show the same target offset in both rounds. */
    struct offset_interval round1, round2;
    assert(interval_strictly_within_tolerance((struct offset_interval){-41, -39}, 100));
    assert(interval_strictly_within_tolerance((struct offset_interval){39, 41}, 100));
    assert(normalize_to_cpu0((struct offset_interval){-41, -39},
                             (struct offset_interval){-41, -39}, &round1));
    assert(normalize_to_cpu0((struct offset_interval){39, 41},
                             (struct offset_interval){39, 41}, &round2));
    assert(cross_round_evaluate(round1, round2, dmax, &change) == DRIFT_ACCEPT);
    assert(cross_round_evaluate((struct offset_interval){-41, -39},
                                (struct offset_interval){39, 41},
                                dmax, &change) == DRIFT_REJECT);

    /* Low drift inside bracket uncertainty cannot be measured exactly; only bounded. */
    assert(cross_round_evaluate((struct offset_interval){-4, 0},
                                (struct offset_interval){0, 4},
                                9, &change) == DRIFT_ACCEPT);
    assert(change.low == 0 && change.high == 8);
}

int main(void)
{
    synchronized_case();
    accept_zero_skew_with_wide_samples_case();
    reject_fixed_skew_case();
    reject_disjoint_intervals_case();
    reject_drift_case();
    reject_nonmonotonic_case();
    all_online_cpus_case();
    reject_wide_bracket_case();
    reject_reference_rollback_case();
    accept_tolerance_boundary_case();
    overlap_vs_strict_bound_case();
    drift_hidden_by_bracket_uncertainty_case();
    cross_round_drift_cases();
    puts("PASS: intersection and cross-round models enforce strict offset/drift bounds, normalize references, and retry uncertainty only");
    return 0;
}
