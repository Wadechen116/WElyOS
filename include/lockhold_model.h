#ifndef WELYOS_LOCKHOLD_MODEL_H
#define WELYOS_LOCKHOLD_MODEL_H
/* I-055 純模型。backend 保證本 CPU IRQ 排他，並以 seqlock 發布副本。
 * 遠端不得直接呼叫本模型。begin/stop 僅在未持有目標鎖的安全點呼叫。
 * 只記成功 acquire 到 release hook 的內部區間，不代表硬體持鎖時間上界。
 * 不使用除法、配置、I/O、原子或 kernel API；跨執行緒交接以 CPU+id 配對。
 */
#include <stdint.h>

#define LH_SCHED 1u
#define LH_TIME  2u
#define LH_LOCKS 2u
#define LH_RING  16u

enum lh_phase { LH_OFF, LH_ON, LH_DRAINING, LH_FROZEN };
enum lh_flag {
    LH_INCONSISTENT = 1u << 0,
    LH_UNSUPPORTED_SOURCE = 1u << 1,
    LH_SOURCE_CHANGED = 1u << 2,
    LH_BACKWARD = 1u << 3,
    LH_COUNT_SAT = 1u << 4,
    LH_SUM_SAT = 1u << 5,
    LH_RAW_OVERWRITTEN = 1u << 6,
    LH_PANIC = 1u << 7
};
struct lh_stamp {
    uint64_t count;
    uint32_t source, generation;
    uint32_t valid; /* backend: 合格來源、前後來源及 generation 一致且有界讀取 */
};
struct lh_raw {
    uint32_t no;
    struct lh_stamp start, end;
};
struct lh_stat {
    uint32_t count, invalid, flags;
    uint64_t sum, max;
    struct lh_raw ring[LH_RING];
};
struct lh_token { uint32_t active; struct lh_stamp start; };
struct lh_model {
    uint32_t session, phase, flags;
    struct lh_stamp source;
    struct lh_token token[LH_LOCKS];
    struct lh_stat stat[LH_LOCKS];
};

static inline void lh_fail(struct lh_model *m, uint32_t reason)
{
    m->flags |= reason;
    if (m->phase == LH_ON) m->phase = LH_DRAINING;
}
static inline int lh_active(const struct lh_model *m)
{
    return m->token[0].active || m->token[1].active;
}
static inline int lh_same_source(struct lh_stamp a, struct lh_stamp b)
{
    return a.valid && b.valid && a.source == b.source && a.generation == b.generation;
}
/* session 必須非零且不重用；backend 負責跨 CPU ARMING/ACK 的全域協議。
 * 拒絕覆用 ON/DRAINING，即使 active 暫時為零也不能偷偷重設一個 session。
 */
static inline int lh_begin(struct lh_model *m, uint32_t session, struct lh_stamp source)
{
    if (lh_active(m) || m->phase == LH_ON || m->phase == LH_DRAINING ||
        !session || session <= m->session) {
        lh_fail(m, LH_INCONSISTENT);
        return 0;
    }
    if (!source.valid) {
        lh_fail(m, LH_UNSUPPORTED_SOURCE);
        return 0;
    }
    *m = (struct lh_model){0};
    m->session = session;
    m->source = source;
    m->phase = LH_ON;
    return 1;
}
/* 只有安全點才可 FROZEN/ACK。異常 active 不清除，unlock 仍須自然配對。 */
static inline int lh_stop(struct lh_model *m, struct lh_stamp source)
{
    /* 即使最後一筆 release 之後才切源，結束安全點仍須保留證據。 */
    if (m->phase != LH_OFF && !lh_same_source(m->source, source))
        lh_fail(m, LH_SOURCE_CHANGED | (source.valid ? 0u : LH_UNSUPPORTED_SOURCE));
    if (lh_active(m)) {
        lh_fail(m, LH_INCONSISTENT);
        return 0;
    }
    if (m->phase == LH_OFF) return 0;
    m->phase = LH_FROZEN;
    return 1;
}
static inline void lh_acquire(struct lh_model *m, uint32_t id, struct lh_stamp t)
{
    if (m->phase != LH_ON) return;
    if (id < 1 || id > LH_LOCKS) { lh_fail(m, LH_INCONSISTENT); return; }
    struct lh_token *token = &m->token[id - 1];
    if (token->active) { lh_fail(m, LH_INCONSISTENT); return; }
    if (!lh_same_source(m->source, t)) {
        lh_fail(m, t.valid ? LH_SOURCE_CHANGED : LH_UNSUPPORTED_SOURCE);
        return;
    }
    token->start = t;
    token->active = 1;
}
static inline void lh_release(struct lh_model *m, uint32_t id, struct lh_stamp t)
{
    if (id < 1 || id > LH_LOCKS) { lh_fail(m, LH_INCONSISTENT); return; }
    struct lh_token *token = &m->token[id - 1];
    if (!token->active) {
        if (m->phase == LH_ON) lh_fail(m, LH_INCONSISTENT);
        return;
    }
    struct lh_stamp start = token->start;
    token->active = 0; /* DRAINING 也必須清除舊 token，不受 accept_new 限制 */
    struct lh_stat *s = &m->stat[id - 1];
    uint32_t reason = m->flags & LH_PANIC;
    if (!lh_same_source(start, t)) reason |= LH_SOURCE_CHANGED;
    if (!t.valid) reason |= LH_UNSUPPORTED_SOURCE;
    if (!reason && t.count < start.count) reason |= LH_BACKWARD;
    if (reason) {
        if (s->invalid != UINT32_MAX) s->invalid++;
        else reason |= LH_COUNT_SAT;
        s->flags |= reason;
        lh_fail(m, reason);
        return;
    }
    uint64_t duration = t.count - start.count;
    if (s->count == UINT32_MAX) {
        s->flags |= LH_COUNT_SAT;
        lh_fail(m, LH_COUNT_SAT);
        return;
    }
    uint32_t n = s->count++;
    if (n >= LH_RING) s->flags |= LH_RAW_OVERWRITTEN;
    s->ring[n & (LH_RING - 1)] = (struct lh_raw){n, start, t};
    if (duration > s->max) s->max = duration;
    if (UINT64_MAX - s->sum < duration) {
        s->sum = UINT64_MAX;
        s->flags |= LH_SUM_SAT;
    } else s->sum += duration;
    m->flags |= s->flags;
}
#endif
