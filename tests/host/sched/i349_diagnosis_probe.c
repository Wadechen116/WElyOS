/*
 * Host-side model of the I-349 cpu0-stall diagnostics.
 *
 * This model checks diagnostic classification only.  A target timeout always
 * remains a failure, including ENVIRONMENT_SUSPECT; no evidence here waives a
 * kernel correctness assertion.  The 2-tick lag margin, three-window run,
 * 50-ms windows, and 80% progress threshold are provisional policy inputs,
 * not calibrated production limits.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#define MAX_WINDOWS 8
#define WINDOW_MS 50 /* matches the current 300-ms kernel probe: six full windows */

enum diagnostic {
    DIAG_OK,
    DIAG_ENVIRONMENT_SUSPECT,
    DIAG_KERNEL_CANDIDATE,
    DIAG_INCONCLUSIVE,
};

enum scope {
    SCOPE_UNKNOWN,
    SCOPE_NONE,
    SCOPE_SYSTEM_WIDE,
    SCOPE_TARGET_LOCAL,
    SCOPE_CONTROL_ONLY,
    SCOPE_NO_CONTROL,
};

enum lag_class {
    LAG_UNKNOWN,
    LAG_NONE,
    LAG_SPIKE,
    LAG_SUSTAINED,
};

enum host_elapsed_class {
    HOST_ELAPSED_UNKNOWN,
    HOST_ELAPSED_NORMAL,
    HOST_ELAPSED_SLOW,
};

struct window {
    uint64_t counter_delta;
    uint64_t committed_tick_delta;
    uint32_t iterations;
};

struct evidence {
    const struct window *windows;
    unsigned window_count;
    uint64_t counts_per_tick;
    uint32_t baseline_iterations;
    int target_over_limit;
    int control_over_limit;
    int have_control;
    enum host_elapsed_class host_elapsed;
};

struct diagnosis {
    enum diagnostic label;
    enum scope affected_scope;
    enum lag_class lag;
    int test_failed;
    unsigned lag_windows;
    unsigned longest_lag_run;
    unsigned min_progress_percent;
    int progress_known;
};

/* Match the proposed clock-relative rule without overflowing a multiply. */
static int window_lags(const struct window *w, uint64_t iv)
{
    uint64_t expected_with_margin;

    if (!iv)
        return 0;
    if (w->committed_tick_delta > UINT64_MAX - 2)
        return 0; /* malformed/unrepresentable evidence is not positive proof */
    expected_with_margin = w->committed_tick_delta + 2;
    if (expected_with_margin > UINT64_MAX / iv)
        return 0;
    return w->counter_delta > expected_with_margin * iv;
}

static struct diagnosis diagnose(const struct evidence *e)
{
    struct diagnosis d = {
        .label = DIAG_INCONCLUSIVE,
        .affected_scope = SCOPE_UNKNOWN,
        .lag = LAG_UNKNOWN,
        .test_failed = !!e->target_over_limit,
        .min_progress_percent = 0,
        .progress_known = 0,
    };
    unsigned run = 0;

    if (e->window_count == 0 || e->window_count > MAX_WINDOWS || !e->windows)
        return d;
    if (!e->counts_per_tick)
        return d;

    d.affected_scope = !e->have_control ? SCOPE_NO_CONTROL :
        e->target_over_limit && e->control_over_limit ? SCOPE_SYSTEM_WIDE :
        e->target_over_limit ? SCOPE_TARGET_LOCAL :
        e->control_over_limit ? SCOPE_CONTROL_ONLY : SCOPE_NONE;

    d.progress_known = e->baseline_iterations != 0;
    d.min_progress_percent = 100;
    for (unsigned i = 0; i < e->window_count; i++) {
        const struct window *w = &e->windows[i];
        if (window_lags(w, e->counts_per_tick)) {
            d.lag_windows++;
            run++;
            if (run > d.longest_lag_run)
                d.longest_lag_run = run;
        } else {
            run = 0;
        }
        if (d.progress_known) {
            uint64_t pct = (uint64_t)w->iterations * 100 / e->baseline_iterations;
            if (pct > 100)
                pct = 100;
            if (pct < d.min_progress_percent)
                d.min_progress_percent = (unsigned)pct;
        }
    }

    d.lag = d.longest_lag_run >= 3 ? LAG_SUSTAINED :
            d.lag_windows ? LAG_SPIKE : LAG_NONE;
    if (!e->target_over_limit) {
        d.label = DIAG_OK;
    } else if (d.longest_lag_run >= 3 && e->have_control &&
               e->control_over_limit && d.progress_known &&
               d.min_progress_percent < 80 &&
               e->host_elapsed == HOST_ELAPSED_SLOW) {
        d.label = DIAG_ENVIRONMENT_SUSPECT;
    } else if ((e->have_control && !e->control_over_limit) ||
               (d.lag_windows == 0 && d.progress_known &&
                d.min_progress_percent >= 80)) {
        d.label = DIAG_KERNEL_CANDIDATE;
    }
    return d;
}

static struct window steady_lag[] = {
    { .counter_delta = 1000, .committed_tick_delta = 5, .iterations = 70 },
    { .counter_delta = 1000, .committed_tick_delta = 5, .iterations = 75 },
    { .counter_delta = 1000, .committed_tick_delta = 5, .iterations = 72 },
};

static struct evidence sample(struct window *w, unsigned n, uint32_t baseline,
                              int target, int control, int have_control)
{
    return (struct evidence){
        .windows = w, .window_count = n, .counts_per_tick = 100,
        .baseline_iterations = baseline,
        .target_over_limit = target,
        .control_over_limit = control,
        .have_control = have_control,
        .host_elapsed = HOST_ELAPSED_UNKNOWN,
    };
}

static void assert_diag(struct evidence e, enum diagnostic expected,
                        int remains_failure)
{
    struct diagnosis d = diagnose(&e);
    assert(d.label == expected);
    assert(d.test_failed == remains_failure);
}

static void scenario_cases(void)
{
    struct evidence e;
    struct diagnosis d;

    /* (a) One isolated lag window followed by recovery is a spike. */
    struct window spike[] = {
        { .counter_delta = 1000, .committed_tick_delta = 5, .iterations = 70 },
        { .counter_delta = 500, .committed_tick_delta = 5, .iterations = 100 },
        { .counter_delta = 500, .committed_tick_delta = 5, .iterations = 100 },
    };
    e = sample(spike, 3, 100, 1, 1, 1);
    e.host_elapsed = HOST_ELAPSED_SLOW;
    d = diagnose(&e);
    assert(d.label == DIAG_INCONCLUSIVE && d.lag == LAG_SPIKE);
    assert(d.longest_lag_run == 1);
    assert(d.test_failed);

    /* (b) Multiple small deficits form SUSTAINED_LAG; no timeout means no
     * test failure and no environment attribution. */
    e = sample(steady_lag, 3, 100, 0, 0, 1);
    d = diagnose(&e);
    assert(d.lag == LAG_SUSTAINED && d.label == DIAG_OK && !d.test_failed);

    /* (c) Target-only lateness is a kernel-candidate diagnostic. */
    e = sample(steady_lag, 3, 100, 1, 0, 1);
    e.host_elapsed = HOST_ELAPSED_NORMAL;
    d = diagnose(&e);
    assert(d.label == DIAG_KERNEL_CANDIDATE);
    assert(d.affected_scope == SCOPE_TARGET_LOCAL && d.test_failed);

    /* (d) Both sleepers and clock-relative lag are slow, but host elapsed is
     * normal: retain a failure without blaming the host. */
    e = sample(steady_lag, 3, 100, 1, 1, 1);
    e.host_elapsed = HOST_ELAPSED_NORMAL;
    assert_diag(e, DIAG_INCONCLUSIVE, 1);
    assert(diagnose(&e).affected_scope == SCOPE_SYSTEM_WIDE);

    /* (e) Independent host elapsed evidence adds ENVIRONMENT_SUSPECT, but it
     * never converts the original timeout into a pass. */
    e.host_elapsed = HOST_ELAPSED_SLOW;
    d = diagnose(&e);
    assert(d.label == DIAG_ENVIRONMENT_SUSPECT && d.test_failed);

    /* Lower progress by itself is not an environment classification. */
    for (unsigned i = 0; i < 3; i++)
        steady_lag[i].iterations = 76;
    e = sample(steady_lag, 3, 100, 1, 1, 1);
    e.host_elapsed = HOST_ELAPSED_NORMAL;
    assert_diag(e, DIAG_INCONCLUSIVE, 1);
    for (unsigned i = 0; i < 3; i++)
        steady_lag[i].iterations = 70;

    /* (f) Missing reference, clock, host elapsed, or progress data is
     * inconclusive; it cannot be used to waive a failure. */
    e = sample(steady_lag, 3, 0, 1, 1, 0);
    e.host_elapsed = HOST_ELAPSED_SLOW;
    assert_diag(e, DIAG_INCONCLUSIVE, 1);
    e = sample(steady_lag, 3, 100, 1, 1, 0);
    e.host_elapsed = HOST_ELAPSED_SLOW;
    assert_diag(e, DIAG_INCONCLUSIVE, 1);
    e.counts_per_tick = 0;
    assert_diag(e, DIAG_INCONCLUSIVE, 1);
    e = sample(steady_lag, 3, 100, 1, 1, 1);
    assert_diag(e, DIAG_INCONCLUSIVE, 1); /* host elapsed unknown */

    /* A healthy probe can run below the provisional progress threshold and
     * still pass; this mirrors the i386 no-injection 76% observation. */
    struct window healthy[] = {
        { .counter_delta = 700, .committed_tick_delta = 5, .iterations = 76 },
        { .counter_delta = 700, .committed_tick_delta = 5, .iterations = 76 },
        { .counter_delta = 700, .committed_tick_delta = 5, .iterations = 76 },
    };
    e = sample(healthy, 3, 100, 0, 0, 1);
    assert_diag(e, DIAG_OK, 0);
    /* Claude's i386 no-injection sample reached 76% progress while another
     * architecture was under load. A sub-80% progress value alone must not
     * create an environment label or change a passing result. */
    assert(diagnose(&e).min_progress_percent == 76);

    /* An unavailable or oversized sample set is inconclusive, never a waiver. */
    e = sample(healthy, 0, 100, 1, 1, 1);
    assert_diag(e, DIAG_INCONCLUSIVE, 1);
}

int main(void)
{
    scenario_cases();
    puts("I-349: classification model cases passed (diagnostic labels never waive failure)");
    return 0;
}
