/* Host tests for the shared I-349 pure classifier. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "i349_classify.h"

static void fill_sleeper(struct i349_sleeper *s, uint32_t total,
                         uint32_t over, uint64_t base, uint64_t late);

static void init_obs(struct i349_obs *o)
{
    *o = (struct i349_obs){0};
    o->iv = 100;
    o->nwin = 3;
    o->have_control = 1;
    o->base_iter.known = 1;
    o->base_iter.v = 100;
    o->inj = I349_INJ_NONE;
    assert(i349_params_default(&o->p, o->iv) == 0);
    for (unsigned i = 0; i < o->nwin; i++) {
        o->win[i] = (struct i349_win){
            .dc = 500,
            .dt = 5,
            .iter = 100,
            .valid = 1,
        };
    }
    fill_sleeper(&o->target, 12, 0, 1000, 1500);
    fill_sleeper(&o->control, 12, 0, 1000, 1500);
}

/* Populate a coherent retained sample set. over is the kernel's exact count
 * using d*1000 > cs_hz*CS_STRICT_MS, not a separately inferred threshold. */
static void fill_sleeper(struct i349_sleeper *s, uint32_t total,
                         uint32_t over, uint64_t base, uint64_t late)
{
    assert(total <= I349_MAX_SAMPLES);
    assert(over <= total);
    *s = (struct i349_sleeper){0};
    s->total = total;
    s->kept = total;
    s->over = over;
    for (uint32_t i = 0; i < total; i++)
        s->d[i] = i < over ? late : base;
}

static struct i349_diag classify(const struct i349_obs *o)
{
    struct i349_diag d;
    assert(i349_classify(o, &d) == 0);
    return d;
}

static void progress_ratio_boundaries(void)
{
    struct i349_obs o;
    struct i349_diag d;
    uint32_t eighty = (uint32_t)(((uint64_t)UINT32_MAX * 80u) / 100u);

    /* Near UINT32_MAX the old common-right-shift method rounded this ratio
     * up to 100%; the exact integer percentage is 99%. */
    assert(i349_progress_pct(UINT32_MAX - 1u, UINT32_MAX) == 99u);
    assert(i349_progress_pct(eighty, UINT32_MAX) == 80u);
    assert(i349_progress_pct(eighty - 1u, UINT32_MAX) == 79u);
    assert(i349_progress_pct(0u, UINT32_MAX) == 0u);
    assert(i349_progress_pct(UINT32_MAX, UINT32_MAX) == 100u);

    /* The classifier's environment threshold must distinguish exactly 80%
     * from the immediately lower count, even at a UINT32_MAX baseline. */
    init_obs(&o);
    o.base_iter.v = UINT32_MAX;
    fill_sleeper(&o.target, 20, 1, 1000, 1500);
    fill_sleeper(&o.control, 20, 1, 1000, 1500);
    for (unsigned i = 0; i < o.nwin; i++) {
        o.win[i].dc = 701; /* (dt + lag_tick) * iv + 1 */
        o.win[i].iter = eighty;
    }
    d = classify(&o);
    assert(d.progress_known && d.progress_min_pct == 80u);
    assert(d.lag == I349_LAG_SUSTAINED);
    assert(d.label == I349_LABEL_INCONCLUSIVE);

    for (unsigned i = 0; i < o.nwin; i++)
        o.win[i].iter = eighty - 1u;
    d = classify(&o);
    assert(d.progress_known && d.progress_min_pct == 79u);
    assert(d.label == I349_LABEL_ENVIRONMENT_SUSPECT);
}

/* Reproduce only the old faulty fallback predicate as a negative-control
 * witness. The fixture must satisfy it, proving that the old precedence
 * would have produced KERNEL-CANDIDATE for this SYSTEM-WIDE evidence. */
static int legacy_precedence_would_mislabel(const struct i349_obs *o,
                                             const struct i349_diag *d)
{
    return o->target.over && d->lag_windows == 0 && d->progress_known &&
           d->progress_min_pct >= 80u;
}

struct historical_case {
    const char *name;
    uint64_t iv;
    uint32_t wakes;
    uint32_t target_over;
    uint32_t control_over;
    uint32_t progress_pct;
    uint64_t dc[5];
    uint64_t dt[5];
    uint32_t iter[5];
};

/* These five rows preserve the logged calibration values. Individual sleep
 * durations were not logged; fill_sleeper creates internally coherent
 * synthetic durations solely to exercise the p50/p95 output. The historical
 * assertions rely on the recorded wake/over counts and SYSTEM_WIDE scope. */
static const struct historical_case historical[] = {
    {"riscv64 steady run 1", 10000, 22, 7, 7, 100,
     {500001,500001,500000,500000,500000}, {50,50,50,50,49},
     {212831,215012,212962,212975,211710}},
    {"riscv64 steady run 4", 10000, 21, 10, 10, 100,
     {500001,500000,500001,500001,500001}, {51,49,49,51,51},
     {206302,209098,201358,218009,200743}},
    {"riscv64 steady run 9", 10000, 22, 5, 7, 99,
     {500002,500000,500001,500002,500001}, {50,48,51,51,50},
     {185297,188894,187979,192100,203148}},
    {"aarch64 steady run 4", 62500, 22, 9, 9, 86,
     {3125012,3125025,3125013,3125000,3125012}, {50,49,51,51,49},
     {124629,136000,142900,134517,139163}},
    {"aarch64 steady run 7", 62500, 22, 8, 9, 88,
     {3125006,3125000,3125012,3125007,3125000}, {49,49,52,49,51},
     {134780,136942,137085,138961,137602}},
};

static void historical_system_wide_regressions(void)
{
    for (unsigned c = 0; c < sizeof(historical) / sizeof(historical[0]); c++) {
        const struct historical_case *h = &historical[c];
        struct i349_obs o;
        struct i349_diag d;
        init_obs(&o);
        o.iv = h->iv;
        o.nwin = 5;
        assert(i349_params_default(&o.p, o.iv) == 0);
        o.base_iter.v = (h->iter[0] / h->progress_pct) * 100u;
        for (unsigned i = 0; i < o.nwin; i++) {
            o.win[i] = (struct i349_win){
                .dc = h->dc[i], .dt = h->dt[i], .iter = h->iter[i], .valid = 1,
            };
        }
        fill_sleeper(&o.target, h->wakes, h->target_over,
                     10 * h->iv, 15 * h->iv);
        fill_sleeper(&o.control, h->wakes, h->control_over,
                     10 * h->iv, 15 * h->iv);

        d = classify(&o);
        assert(d.scope == I349_SCOPE_SYSTEM_WIDE);
        assert(d.label != I349_LABEL_TARGET_PATH_CANDIDATE);
        assert(d.label == I349_LABEL_INCONCLUSIVE);
        assert(legacy_precedence_would_mislabel(&o, &d));
        assert(o.target.over == h->target_over);
        assert(o.control.over == h->control_over);
        assert(d.reason & I349_R_SYSTEM_WIDE_DELAY);
        assert(d.lag == I349_LAG_NONE);
    }
}

static void scope_and_label_cases(void)
{
    struct i349_obs o;
    struct i349_diag d;

    init_obs(&o);
    fill_sleeper(&o.target, 12, 0, 1000, 1500);
    fill_sleeper(&o.control, 12, 0, 1000, 1500);
    d = classify(&o);
    assert(d.scope == I349_SCOPE_NONE && d.label == I349_LABEL_OK);

    o.have_control = 0;
    d = classify(&o);
    assert(d.scope == I349_SCOPE_NO_CONTROL && d.label == I349_LABEL_OK);

    o.have_control = 1;
    o.control.over = 1;
    d = classify(&o);
    assert(d.scope == I349_SCOPE_CONTROL_ONLY);
    assert(d.label == I349_LABEL_OK); /* target did not exceed its limit */

    /* Three consecutive windows over the provisional two-tick margin. */
    for (unsigned i = 0; i < 3; i++) {
        o.win[i].dc = 900;
        o.win[i].iter = 70;
    }
    fill_sleeper(&o.target, 12, 1, 1000, 1500);
    fill_sleeper(&o.control, 12, 1, 1000, 1500);
    d = classify(&o);
    assert(d.lag == I349_LAG_SUSTAINED);
    assert(d.scope == I349_SCOPE_SYSTEM_WIDE);
    assert(d.label == I349_LABEL_ENVIRONMENT_SUSPECT);

    /* Unknown progress is not treated as normal and blocks environment blame. */
    o.base_iter.known = 0;
    d = classify(&o);
    assert(!d.progress_known);
    assert(d.reason & I349_R_PROGRESS_UNKNOWN);
    assert(d.label == I349_LABEL_INCONCLUSIVE);

    /* Target-local evidence remains a candidate even if optional progress is
     * unknown; the reason states that evidence is timeout-distribution only. */
    init_obs(&o);
    o.base_iter.known = 0;
    fill_sleeper(&o.target, 20, 2, 1100, 1501);
    fill_sleeper(&o.control, 20, 0, 1100, 1501);
    d = classify(&o);
    assert(d.scope == I349_SCOPE_TARGET_LOCAL);
    assert(d.label == I349_LABEL_TARGET_PATH_CANDIDATE);
    assert(d.reason & I349_R_TIMEOUT_DIST_ONLY);
    assert(d.reason & I349_R_PROGRESS_UNKNOWN);
}

static void percentile_and_gap_boundaries(void)
{
    struct i349_obs o;
    struct i349_diag d;
    uint64_t original[I349_MAX_SAMPLES];

    init_obs(&o);
    /* n=20 nearest rank: p50 is item 10 (index 9), p95 item 19 (index 18).
     * Two late samples make p95 cross the 4-tick gap by one count. */
    fill_sleeper(&o.target, 20, 2, 1100, 1501);
    fill_sleeper(&o.control, 20, 0, 1100, 1501);
    for (unsigned i = 0; i < 20; i++)
        original[i] = o.target.d[i];
    d = classify(&o);
    assert(d.target_percentiles_known && d.control_percentiles_known);
    assert(d.target_p50 == 1100 && d.control_p50 == 1100);
    assert(d.target_p95 == 1501 && d.control_p95 == 1100);
    assert(d.scope == I349_SCOPE_TARGET_LOCAL);
    assert(d.label == I349_LABEL_TARGET_PATH_CANDIDATE);
    for (unsigned i = 0; i < 20; i++)
        assert(o.target.d[i] == original[i]); /* classifier sorts a local copy */

    /* Equal-to-gap is not strictly greater and cannot establish TARGET_LOCAL. */
    fill_sleeper(&o.target, 20, 2, 1100, 1500);
    fill_sleeper(&o.control, 20, 0, 1100, 1500);
    d = classify(&o);
    assert(d.target_p95 == d.control_p95 + o.p.gap_counts);
    assert(d.scope == I349_SCOPE_INCONCLUSIVE);
    assert(d.label == I349_LABEL_INCONCLUSIVE);

    /* Only one high sample leaves the nearest-rank p95 at the lower value. */
    fill_sleeper(&o.target, 20, 1, 1100, 1600);
    fill_sleeper(&o.control, 20, 0, 1100, 1600);
    d = classify(&o);
    assert(d.target_p95 == 1100);
    assert(d.scope == I349_SCOPE_INCONCLUSIVE);

    /* Fewer than the configured 10 control samples is inconclusive. */
    fill_sleeper(&o.target, 20, 2, 1100, 1501);
    fill_sleeper(&o.control, 9, 0, 1100, 1501);
    d = classify(&o);
    assert(d.scope == I349_SCOPE_INCONCLUSIVE);
    assert(d.reason & I349_R_FEW_CONTROL);
}

static void bounded_and_invalid_inputs(void)
{
    struct i349_obs o;
    struct i349_diag d;

    init_obs(&o);
    fill_sleeper(&o.target, 0, 0, 1000, 1500);
    fill_sleeper(&o.control, 12, 0, 1000, 1500);
    d = classify(&o);
    assert(d.scope == I349_SCOPE_INVALID);
    assert(d.label == I349_LABEL_INCONCLUSIVE);
    assert(d.reason & I349_R_MISSING_TARGET_SAMPLES);

    init_obs(&o);
    /* For n=64, nearest-rank p95 is item 61; four late values place it at
     * 1501, above the 400-count gap from the control p95 of 1000. */
    fill_sleeper(&o.target, 64, 4, 1000, 1501);
    fill_sleeper(&o.control, 64, 0, 1000, 1500);
    d = classify(&o);
    assert(d.target_p95 == 1501 && d.control_p95 == 1000);
    assert(d.scope == I349_SCOPE_TARGET_LOCAL);

    /* Truncated observations preserve total/over counts but cannot establish
     * target-local scope. The retained arrays remain bounded at 64 entries. */
    init_obs(&o);
    fill_sleeper(&o.target, 64, 2, 1100, 1501);
    fill_sleeper(&o.control, 64, 0, 1100, 1501);
    o.target.total = 65;
    o.target.truncated = 1;
    d = classify(&o);
    assert(d.scope == I349_SCOPE_INCONCLUSIVE);
    assert(d.reason & I349_R_TRUNCATED);
    assert(d.label != I349_LABEL_TARGET_PATH_CANDIDATE);

    /* The retained prefix must be full at the bound: silent under-retention
     * is malformed, even if the producer set truncated correctly. */
    init_obs(&o);
    fill_sleeper(&o.target, 64, 2, 1100, 1501);
    fill_sleeper(&o.control, 20, 0, 1100, 1501);
    o.target.total = 65;
    o.target.kept = 63;
    o.target.truncated = 1;
    d = classify(&o);
    assert(d.scope == I349_SCOPE_INVALID);
    assert(d.label == I349_LABEL_INCONCLUSIVE);
    assert(d.reason & I349_R_BAD_OBSERVATION);

    init_obs(&o);
    fill_sleeper(&o.target, 20, 2, 1100, 1501);
    fill_sleeper(&o.control, 64, 0, 1100, 1501);
    o.control.total = 65;
    o.control.truncated = 1;
    d = classify(&o);
    assert(d.scope == I349_SCOPE_INCONCLUSIVE);
    assert(d.reason & I349_R_TRUNCATED);

    init_obs(&o);
    o.win[1].valid = 0;
    d = classify(&o);
    assert(d.scope == I349_SCOPE_INVALID);
    assert(d.reason & I349_R_INVALID_WINDOW);

    init_obs(&o);
    o.iv = 0;
    d = classify(&o);
    assert(d.scope == I349_SCOPE_INVALID);

    init_obs(&o);
    o.nwin = 0;
    d = classify(&o);
    assert(d.scope == I349_SCOPE_INVALID);

    init_obs(&o);
    o.target.over = o.target.total + 1;
    d = classify(&o);
    assert(d.scope == I349_SCOPE_INVALID);

    init_obs(&o);
    o.win[0].dt = UINT64_MAX;
    d = classify(&o);
    assert(d.scope == I349_SCOPE_INVALID);
    assert(d.reason & I349_R_OVERFLOW);

    init_obs(&o);
    o.p.gap_counts = 4;
    fill_sleeper(&o.target, 20, 2, UINT64_MAX, UINT64_MAX);
    fill_sleeper(&o.control, 20, 0, UINT64_MAX - 1, UINT64_MAX);
    d = classify(&o);
    assert(d.scope == I349_SCOPE_INVALID);
    assert(d.reason & I349_R_OVERFLOW);

    assert(i349_params_default(&o.p, 0) != 0);
    assert(i349_params_default(&o.p, UINT64_MAX / 4 + 1) != 0);
}

static void injection_and_precedence_negative_control(void)
{
    struct i349_obs o;
    struct i349_diag d;

    /* Without a control, a target timeout is not enough to attribute the
     * scope or produce a target-path candidate. */
    init_obs(&o);
    o.have_control = 0;
    fill_sleeper(&o.target, 20, 2, 1100, 1501);
    d = classify(&o);
    assert(o.target.over == 2);
    assert(d.scope == I349_SCOPE_NO_CONTROL);
    assert(d.label == I349_LABEL_INCONCLUSIVE);
    assert(d.label != I349_LABEL_OK);
    assert(d.label != I349_LABEL_TARGET_PATH_CANDIDATE);
    assert(d.reason & I349_R_NO_CONTROL);

    init_obs(&o);
    o.inj = I349_INJ_SPIKE_VALID;
    fill_sleeper(&o.target, 22, 7, 1000, 1500);
    fill_sleeper(&o.control, 22, 7, 1000, 1500);
    d = classify(&o);
    assert(d.reason & I349_R_INJECTED);

    /* Negative control for the old precedence: the fixture has a timeout in
     * both sleepers and high progress, which made the legacy fallback choose
     * KERNEL-CANDIDATE merely because the lag count was zero. The new rule
     * must choose SYSTEM_WIDE/INCONCLUSIVE before considering that fallback. */
    assert(d.scope == I349_SCOPE_SYSTEM_WIDE);
    assert(d.label == I349_LABEL_INCONCLUSIVE);
    assert(d.label != I349_LABEL_TARGET_PATH_CANDIDATE);
    assert(d.reason & I349_R_SYSTEM_WIDE_DELAY);
    assert(legacy_precedence_would_mislabel(&o, &d));

    /* Invalid, missed, and steady injections are diagnostic annotations;
     * none may erase the target timeout or turn a failing observation into
     * an OK result. */
    {
        static const enum i349_inj injections[] = {
            I349_INJ_SPIKE_INVALID,
            I349_INJ_SPIKE_MISS,
            I349_INJ_STEADY,
        };
        for (unsigned i = 0; i < sizeof(injections) / sizeof(injections[0]); i++) {
            init_obs(&o);
            o.inj = injections[i];
            fill_sleeper(&o.target, 20, 2, 1100, 1501);
            fill_sleeper(&o.control, 20, 0, 1100, 1501);
            d = classify(&o);
            assert(o.target.over == 2);
            assert(d.reason & I349_R_INJECTED);
            assert(d.scope == I349_SCOPE_TARGET_LOCAL);
            assert(d.label == I349_LABEL_TARGET_PATH_CANDIDATE);
            assert(d.label != I349_LABEL_OK);
        }
    }
}

int main(void)
{
    progress_ratio_boundaries();
    historical_system_wide_regressions();
    scope_and_label_cases();
    percentile_and_gap_boundaries();
    bounded_and_invalid_inputs();
    injection_and_precedence_negative_control();
    puts("I-349: shared classifier host cases passed; labels never waive timeout failures");
    return 0;
}
