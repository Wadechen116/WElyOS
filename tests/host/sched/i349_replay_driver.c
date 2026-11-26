/* 只呼叫正式分類器；輸入由 Python 驗證並編成固定十進位協定。 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "i349_classify.h"

static int number(uint64_t *value)
{
    char token[32];
    uint64_t n = 0;
    if (scanf("%31s", token) != 1)
        return 0;
    for (unsigned i = 0; token[i]; ++i) {
        if (token[i] < '0' || token[i] > '9')
            return 0;
        unsigned digit = (unsigned)(token[i] - '0');
        if (n > (UINT64_MAX - digit) / 10)
            return 0;
        n = n * 10 + digit;
    }
    *value = n;
    return 1;
}

#define READ(field, maximum) do { \
    uint64_t v; \
    if (!number(&v) || v > (maximum)) return 2; \
    (field) = v; \
} while (0)

static int sleeper(struct i349_sleeper *s)
{
    READ(s->total, UINT32_MAX);
    READ(s->kept, I349_MAX_SAMPLES);
    READ(s->truncated, UINT8_MAX);
    READ(s->over, UINT32_MAX);
    for (uint32_t i = 0; i < s->kept; ++i)
        READ(s->d[i], UINT64_MAX);
    return 0;
}

int main(void)
{
    struct i349_obs o;
    struct i349_diag d;
    memset(&o, 0, sizeof(o));
    READ(o.iv, UINT64_MAX);
    READ(o.nwin, I349_MAX_WIN);
    for (uint32_t i = 0; i < o.nwin; ++i) {
        READ(o.win[i].dc, UINT64_MAX);
        READ(o.win[i].dt, UINT64_MAX);
        READ(o.win[i].iter, UINT32_MAX);
        READ(o.win[i].valid, UINT8_MAX);
    }
    READ(o.have_control, UINT8_MAX);
    if (sleeper(&o.target) || sleeper(&o.control))
        return 2;
    READ(o.base_iter.known, UINT8_MAX);
    READ(o.base_iter.v, UINT32_MAX);
    /* enum 的 ABI 使用 int；保留未知值供分類器判為 INVALID。 */
    READ(o.inj, INT32_MAX);
    READ(o.p.gap_counts, UINT64_MAX);
    READ(o.p.lag_tick, UINT32_MAX);
    READ(o.p.sustained_run, UINT32_MAX);
    READ(o.p.min_control_samples, UINT32_MAX);
    READ(o.p.progress_pct, UINT32_MAX);
    char extra;
    if (scanf(" %c", &extra) != EOF || i349_classify(&o, &d))
        return 2;
    printf("%u %u %u %u %u %u %u %" PRIu64 " %" PRIu64
           " %u %" PRIu64 " %" PRIu64 " %u %u\n",
           (unsigned)d.lag, (unsigned)d.scope, (unsigned)d.label,
           d.lag_windows, d.longest_run, d.progress_min_pct,
           (unsigned)d.progress_known, d.target_p50, d.target_p95,
           (unsigned)d.target_percentiles_known, d.control_p50,
           d.control_p95, (unsigned)d.control_percentiles_known, d.reason);
    return 0;
}
