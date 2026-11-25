#ifndef WELYOS_I349_CLASSIFY_H
#define WELYOS_I349_CLASSIFY_H

/*
 * I-349 CPU0 停滯自我測試使用的純診斷分類器。
 * 本標頭僅供 kernel/test 與 host 測試共用，不改變自我測試的通過、失敗或 panic 行為。
 * 門檻目前只是診斷輸入，尚未校準成可靠度界限。
 */
#include <stdint.h>

#define I349_MAX_WIN     8u
#define I349_MAX_SAMPLES 64u

struct i349_known_u32 {
    uint8_t known;
    uint32_t v;
};

struct i349_win {
    uint64_t dc;       /* 高精度計數器的差值 */
    uint64_t dt;       /* 已提交的計時器 tick 差值 */
    uint32_t iter;     /* 此時間窗內進度迴圈的執行次數 */
    uint8_t valid;
};

struct i349_sleeper {
    uint32_t total;        /* kernel 測試使用的全部已完成樣本數 */
    uint32_t kept;         /* d[] 實際保留的 duration 數 */
    uint8_t truncated;     /* 僅當 total > kept 時設為真 */
    uint64_t d[I349_MAX_SAMPLES];
    uint32_t over;         /* 完全沿用既有 kernel timeout 條件計算的超時數 */
};

struct i349_params {
    uint64_t gap_counts;          /* target/control p95 的暫定差距 */
    uint32_t lag_tick;            /* 相對時鐘落後量的暫定門檻 */
    uint32_t sustained_run;       /* 判為 SUSTAINED 所需的連續落後時間窗數 */
    uint32_t min_control_samples; /* control 至少須保留的 duration 樣本數 */
    uint32_t progress_pct;        /* 低進度百分比門檻 */
};

enum i349_inj {
    I349_INJ_NONE = 0,
    I349_INJ_SPIKE_VALID,
    I349_INJ_SPIKE_INVALID,
    I349_INJ_SPIKE_MISS,
    I349_INJ_STEADY
};

struct i349_obs {
    uint64_t iv;                  /* 每個 tick 對應的高精度計數器計數 */
    uint32_t nwin;
    struct i349_win win[I349_MAX_WIN];
    uint8_t have_control;
    struct i349_sleeper target;
    struct i349_sleeper control;
    struct i349_known_u32 base_iter;
    enum i349_inj inj;
    struct i349_params p;
};

enum i349_lag {
    I349_LAG_NONE = 0,
    I349_LAG_SPIKE,
    I349_LAG_SUSTAINED,
    I349_LAG_INVALID
};

enum i349_scope {
    I349_SCOPE_INVALID = 0,
    I349_SCOPE_NO_CONTROL,
    I349_SCOPE_NONE,
    I349_SCOPE_SYSTEM_WIDE,
    I349_SCOPE_CONTROL_ONLY,
    I349_SCOPE_TARGET_LOCAL,
    I349_SCOPE_INCONCLUSIVE
};

enum i349_label {
    I349_LABEL_OK = 0,
    I349_LABEL_ENVIRONMENT_SUSPECT,
    I349_LABEL_TARGET_PATH_CANDIDATE,
    I349_LABEL_INCONCLUSIVE
};

enum i349_reason {
    I349_R_TRUNCATED          = 1u << 0,
    I349_R_TIMEOUT_DIST_ONLY  = 1u << 1,
    I349_R_SYSTEM_WIDE_DELAY  = 1u << 2,
    I349_R_OVERFLOW           = 1u << 3,
    I349_R_INVALID_WINDOW     = 1u << 4,
    I349_R_PROGRESS_UNKNOWN   = 1u << 5,
    I349_R_FEW_CONTROL        = 1u << 6,
    I349_R_INJECTED           = 1u << 7,
    I349_R_BAD_OBSERVATION    = 1u << 8,
    I349_R_NO_CONTROL         = 1u << 9,
    I349_R_CONTROL_ONLY       = 1u << 10,
    I349_R_NO_TIMEOUT         = 1u << 11,
    I349_R_MISSING_TARGET_SAMPLES = 1u << 12
};

struct i349_diag {
    enum i349_lag lag;
    enum i349_scope scope;
    enum i349_label label;
    uint32_t lag_windows;
    uint32_t longest_run;
    uint32_t progress_min_pct;
    uint8_t progress_known;
    uint64_t target_p50;
    uint64_t target_p95;
    uint8_t target_percentiles_known;
    uint64_t control_p50;
    uint64_t control_p95;
    uint8_t control_percentiles_known;
    uint32_t reason;
};

/* 參數為 null 時回傳非零；非 null 但格式錯誤的觀測值會透過 diag
 * 回報為 INVALID / INVALID / INCONCLUSIVE，並回傳 0。 */
static inline int i349_params_default(struct i349_params *p, uint64_t iv)
{
    if (!p || !iv || iv > UINT64_MAX / 4u)
        return -1;

    p->gap_counts = iv * 4u;
    p->lag_tick = 2u;
    p->sustained_run = 3u;
    p->min_control_samples = 10u;
    p->progress_pct = 80u;
    return 0;
}

static inline int i349_mul_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    /* GCC 會將此溢位檢查編譯成不含執行期 64 位元除法的程式碼；
     * 專案已在 kernel/clksync.c 使用 __builtin_*_overflow。 */
    return !__builtin_mul_overflow(a, b, out);
}

static inline int i349_add_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (b > UINT64_MAX - a)
        return 0;
    *out = a + b;
    return 1;
}

/* 進度為 floor(iter * 100 / baseline)，最高限制為 100%。
 * 僅用 32 位元商與餘數運算搜尋百分比門檻。 */
static inline uint32_t i349_progress_pct(uint32_t iter, uint32_t baseline)
{
    uint32_t low = 0u;
    uint32_t high = 100u;
    uint32_t q, rem;

    if (!baseline)
        return 0u;
    if (iter >= baseline)
        return 100u;

    q = baseline / 100u;
    rem = baseline % 100u;
    /* 對候選百分比 p，ceil(p*baseline/100) 等於
     * q*p + ceil(rem*p/100)；各項及其總和皆可放入 uint32_t。 */
    while (low + 1u < high) {
        uint32_t mid = low + ((high - low) >> 1);
        uint32_t threshold = q * mid + (rem * mid + 99u) / 100u;
        if (iter >= threshold)
            low = mid;
        else
            high = mid;
    }
    return low;
}

static inline int i349_sleeper_valid(const struct i349_sleeper *s)
{
    uint32_t expected_kept = s->total < I349_MAX_SAMPLES ?
                             s->total : I349_MAX_SAMPLES;

    if (s->kept != expected_kept || s->truncated > 1u ||
        s->truncated != (s->total > I349_MAX_SAMPLES) ||
        s->over > s->total)
        return 0;
    return 1;
}

/* 對區域副本計算 nearest-rank 百分位數，不修改輸入觀測值。
 * rank(q,n) = ceil(q*n/100)，並轉為從零起算的索引。 */
static inline void i349_percentiles(const struct i349_sleeper *s,
                                    uint64_t *p50, uint64_t *p95,
                                    uint8_t *known)
{
    uint64_t sorted[I349_MAX_SAMPLES];
    uint32_t i;

    *p50 = 0;
    *p95 = 0;
    *known = 0;
    if (!s->kept)
        return;

    for (i = 0; i < s->kept; i++) {
        uint64_t value = s->d[i];
        uint32_t j = i;
        while (j && sorted[j - 1u] > value) {
            sorted[j] = sorted[j - 1u];
            j--;
        }
        sorted[j] = value;
    }

    {
        uint32_t n = s->kept;
        uint32_t i50 = (50u * n + 99u) / 100u - 1u;
        uint32_t i95 = (95u * n + 99u) / 100u - 1u;
        *p50 = sorted[i50];
        *p95 = sorted[i95];
        *known = 1u;
    }
}

static inline void i349_invalid(struct i349_diag *d, uint32_t reason)
{
    d->lag = I349_LAG_INVALID;
    d->scope = I349_SCOPE_INVALID;
    d->label = I349_LABEL_INCONCLUSIVE;
    d->reason |= reason;
}

static inline int i349_classify(const struct i349_obs *o, struct i349_diag *d)
{
    uint32_t i, run = 0;
    uint64_t margin, threshold;
    int truncated;

    if (!o || !d)
        return -1;

    d->lag = I349_LAG_INVALID;
    d->scope = I349_SCOPE_INVALID;
    d->label = I349_LABEL_INCONCLUSIVE;
    d->lag_windows = 0;
    d->longest_run = 0;
    d->progress_min_pct = 0;
    d->progress_known = 0;
    d->target_p50 = d->target_p95 = 0;
    d->target_percentiles_known = 0;
    d->control_p50 = d->control_p95 = 0;
    d->control_percentiles_known = 0;
    d->reason = 0;

    if (!o->target.total) {
        i349_invalid(d, I349_R_MISSING_TARGET_SAMPLES);
        return 0;
    }

    if (!o->iv || !o->nwin || o->nwin > I349_MAX_WIN ||
        o->have_control > 1u || o->base_iter.known > 1u ||
        o->inj < I349_INJ_NONE || o->inj > I349_INJ_STEADY ||
        !o->p.sustained_run || !o->p.min_control_samples ||
        o->p.progress_pct > 100u ||
        (o->base_iter.known && !o->base_iter.v) ||
        !i349_sleeper_valid(&o->target) ||
        (o->have_control && !i349_sleeper_valid(&o->control))) {
        i349_invalid(d, I349_R_BAD_OBSERVATION);
        return 0;
    }

    if (o->inj != I349_INJ_NONE)
        d->reason |= I349_R_INJECTED;
    if (o->target.truncated || (o->have_control && o->control.truncated))
        d->reason |= I349_R_TRUNCATED;

    for (i = 0; i < o->nwin; i++) {
        const struct i349_win *w = &o->win[i];
        uint64_t tick_delta;

        if (w->valid != 1u) {
            i349_invalid(d, I349_R_INVALID_WINDOW);
            return 0;
        }
        if (!i349_add_u64(w->dt, o->p.lag_tick, &tick_delta) ||
            !i349_mul_u64(tick_delta, o->iv, &threshold)) {
            i349_invalid(d, I349_R_OVERFLOW);
            return 0;
        }
        if (w->dc > threshold) {
            d->lag_windows++;
            run++;
            if (run > d->longest_run)
                d->longest_run = run;
        } else {
            run = 0;
        }

        if (o->base_iter.known) {
            uint32_t pct = i349_progress_pct(w->iter, o->base_iter.v);
            if (!d->progress_known || pct < d->progress_min_pct)
                d->progress_min_pct = pct;
            d->progress_known = 1u;
        }
    }

    d->lag = d->longest_run >= o->p.sustained_run ? I349_LAG_SUSTAINED :
             d->lag_windows ? I349_LAG_SPIKE : I349_LAG_NONE;
    if (!d->progress_known)
        d->reason |= I349_R_PROGRESS_UNKNOWN;

    i349_percentiles(&o->target, &d->target_p50, &d->target_p95,
                     &d->target_percentiles_known);
    if (o->have_control)
        i349_percentiles(&o->control, &d->control_p50, &d->control_p95,
                         &d->control_percentiles_known);

    /* 範圍判定的優先順序是刻意安排的：即使進度接近基準值，
     * 也不能因此把全域延遲重新標成 target-local。 */
    if (!o->have_control) {
        d->scope = I349_SCOPE_NO_CONTROL;
        d->reason |= I349_R_NO_CONTROL;
    } else if (!o->target.over && !o->control.over) {
        d->scope = I349_SCOPE_NONE;
        d->reason |= I349_R_NO_TIMEOUT;
    } else if (o->target.over && o->control.over) {
        d->scope = I349_SCOPE_SYSTEM_WIDE;
        d->reason |= I349_R_SYSTEM_WIDE_DELAY;
    } else if (!o->target.over && o->control.over) {
        d->scope = I349_SCOPE_CONTROL_ONLY;
        d->reason |= I349_R_CONTROL_ONLY;
    } else {
        d->scope = I349_SCOPE_INCONCLUSIVE;
        if (!d->control_percentiles_known ||
            o->control.kept < o->p.min_control_samples)
            d->reason |= I349_R_FEW_CONTROL;
        truncated = o->target.truncated || o->control.truncated;
        if (!truncated && d->target_percentiles_known &&
            d->control_percentiles_known &&
            o->control.kept >= o->p.min_control_samples) {
            if (!i349_add_u64(d->control_p95, o->p.gap_counts, &margin)) {
                i349_invalid(d, I349_R_OVERFLOW);
                return 0;
            }
            if (d->target_p95 > margin)
                d->scope = I349_SCOPE_TARGET_LOCAL;
        }
    }

    if (!o->target.over) {
        d->label = I349_LABEL_OK;
        return 0;
    }
    if (d->scope == I349_SCOPE_SYSTEM_WIDE &&
        d->lag == I349_LAG_SUSTAINED && d->progress_known &&
        d->progress_min_pct < o->p.progress_pct) {
        d->label = I349_LABEL_ENVIRONMENT_SUSPECT;
        return 0;
    }
    if (d->scope == I349_SCOPE_SYSTEM_WIDE)
        d->reason |= I349_R_SYSTEM_WIDE_DELAY;
    if (d->scope == I349_SCOPE_TARGET_LOCAL && o->target.over) {
        d->label = I349_LABEL_TARGET_PATH_CANDIDATE;
        d->reason |= I349_R_TIMEOUT_DIST_ONLY;
        return 0;
    }
    d->label = I349_LABEL_INCONCLUSIVE;
    return 0;
}

static inline const char *i349_label_name(enum i349_label label)
{
    switch (label) {
    case I349_LABEL_OK: return "OK";
    case I349_LABEL_ENVIRONMENT_SUSPECT: return "ENVIRONMENT-SUSPECT";
    case I349_LABEL_TARGET_PATH_CANDIDATE: return "TARGET-PATH-CANDIDATE";
    case I349_LABEL_INCONCLUSIVE: return "INCONCLUSIVE";
    default: return "INVALID";
    }
}

static inline const char *i349_scope_name(enum i349_scope scope)
{
    switch (scope) {
    case I349_SCOPE_INVALID: return "INVALID";
    case I349_SCOPE_NO_CONTROL: return "NO-CONTROL";
    case I349_SCOPE_NONE: return "NONE";
    case I349_SCOPE_SYSTEM_WIDE: return "SYSTEM-WIDE";
    case I349_SCOPE_CONTROL_ONLY: return "CONTROL-ONLY";
    case I349_SCOPE_TARGET_LOCAL: return "TARGET-LOCAL";
    case I349_SCOPE_INCONCLUSIVE: return "INCONCLUSIVE";
    default: return "INVALID";
    }
}

static inline const char *i349_lag_name(enum i349_lag lag)
{
    switch (lag) {
    case I349_LAG_NONE: return "NONE";
    case I349_LAG_SPIKE: return "SPIKE";
    case I349_LAG_SUSTAINED: return "SUSTAINED";
    case I349_LAG_INVALID: return "INVALID";
    default: return "INVALID";
    }
}

#endif
