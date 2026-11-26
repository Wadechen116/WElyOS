/*
 * irq_late.c - 每顆 CPU 計時器中斷「晚到」的觀測狀態(I-351 第一階段;介面與語義見 include/irq_late.h)
 *
 * 單一寫入者:每顆 CPU 的狀態(st)只由那顆 CPU 自己的計時器中斷(irq_late_note)寫入。
 * irq_late_config 可能由任一顆 CPU 為任一顆呼叫,所以它不寫狀態,只寫該顆的「設定請求」(rq);
 * 目標 CPU 下一次 irq_late_note 開頭看到請求的 gen 變了,才在自己的狀態 seqlock 內套用。
 *
 * 兩把 seqlock(寫者:seq+1 奇 → release 屏障 → 寫欄位 → release 屏障 → seq+1 偶;
 * 讀者:acquire 讀 seq → 讀欄位 → acquire 屏障 → 再讀 seq,相同且偶數才算一致)。
 * C11 依據:讀者讀到寫者屏障之後寫入的任何值時,讀者的 acquire 屏障與寫者前一個 release 屏障同步,
 * 因此寫者的奇數 seq 對讀者第二次讀 seq 可見 → 重試。
 * i386 沒有 64 位元 __atomic(也不可用 libgcc),所以 64 位元欄位一律拆成 hi/lo 兩個 uint32_t,
 * 一致性由 seqlock 保證。全部共享欄位用 RELAXED 的 __atomic_load_n / __atomic_store_n。
 * 不用除法、不配置、不印、不取鎖。
 */
#include <stdint.h>
#include "cpu.h"
#include "irq_late.h"

_Static_assert((IRQL_RING & (IRQL_RING - 1)) == 0 && IRQL_RING > 0, "IRQL_RING 必須是 2 的冪");

#define LD(p)     __atomic_load_n((p), __ATOMIC_RELAXED)
#define ST(p, v)  __atomic_store_n((p), (v), __ATOMIC_RELAXED)

/* 環形紀錄的一筆:kind / flags / catchup_n 打包成一個 32 位元字 */
struct irql_slot {
    uint32_t rec_no;
    uint32_t meta;               /* kind | flags << 8 | catchup_n << 16 */
    uint32_t ref_hi, ref_lo;
    uint32_t ent_hi, ent_lo;
};

/* 本顆 CPU 的狀態:只有本顆的計時器中斷寫 */
struct irql_state {
    uint32_t seq;                /* 奇數 = 寫入中 */
    uint32_t gen;                /* 已套用的請求 gen */
    uint32_t cfg;                /* src_id | kind << 8 | enabled << 16 */
    uint32_t iv_hi, iv_lo;
    uint32_t heartbeat;
    uint32_t total;
    uint32_t rejected;            /* 啟用期間來源或種類不符而丟棄的輸入 */
    uint32_t below_min;           /* 有晚到但低於記錄門檻(只計數) */
    uint32_t last_hi, last_lo;   /* 最後一次入口;0 = 尚未有 */
    struct irql_slot ring[IRQL_RING];
};

/* 設定請求:任一 CPU 的 irq_late_config 寫,目標 CPU 的 irq_late_note 讀 */
struct irql_req {
    uint32_t seq;                /* 請求 seqlock;也當寫者互斥(CAS 偶 → 奇) */
    uint32_t gen_in;             /* seqlock 內的 gen 副本:讓讀到的欄位與 gen 必定成對 */
    uint32_t gen;                /* 發布用:欄位寫完且仍持有奇數 seq 時 release 更新，之後才解鎖 */
    uint32_t cfg;                /* src_id | kind << 8 */
    uint32_t iv_hi, iv_lo;
};

static struct irql_state st[MAX_CPUS];
static struct irql_req rq[MAX_CPUS];

static inline uint64_t mk64(uint32_t hi, uint32_t lo) { return ((uint64_t)hi << 32) | lo; }

void irq_late_config(int cpu, uint8_t src_id, uint8_t kind, uint64_t iv)
{
    if (cpu < 0 || cpu >= MAX_CPUS)
        return;
    struct irql_req *r = &rq[cpu];
    /* 寫者互斥:config 不在中斷內,可等待;同一顆只有開機 / 來源切換會呼叫,競爭極少 */
    uint32_t s;
    for (;;) {
        s = LD(&r->seq);
        if (!(s & 1) && __atomic_compare_exchange_n(&r->seq, &s, s + 1, 0,
                                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))   /* 取得互斥:看得到前一個寫者的全部寫入 */
            break;
        arch_cpu_relax();
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    uint32_t g = LD(&r->gen_in) + 1;
    ST(&r->gen_in, g);
    ST(&r->cfg, (uint32_t)src_id | (uint32_t)kind << 8);
    ST(&r->iv_hi, (uint32_t)(iv >> 32));
    ST(&r->iv_lo, (uint32_t)iv);
    /* 發布 gen 必須在仍持有互斥時做(Codex 057):若先放開 seq 再寫 gen,W1 放開後被搶占、W2 發布 gen2,
       W1 恢復時會把 gen 寫回 gen1 — 版本倒退。目標 CPU 看到 gen 變了才去讀請求;此時 seq 仍是奇數,
       req_read 會失敗、留到下一次中斷,不會讀到寫一半的請求 */
    __atomic_store_n(&r->gen, g, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&r->seq, s + 2, __ATOMIC_RELEASE);
}

/* 在請求 seqlock 下讀一份一致的請求;只試一次(中斷內不迴圈),不一致就等下一次中斷 */
static int req_read(const struct irql_req *r, uint32_t *gen, uint32_t *cfg, uint32_t *hi, uint32_t *lo)
{
    uint32_t s = __atomic_load_n(&r->seq, __ATOMIC_ACQUIRE);
    if (s & 1)
        return 0;
    *gen = LD(&r->gen_in);
    *cfg = LD(&r->cfg);
    *hi = LD(&r->iv_hi);
    *lo = LD(&r->iv_lo);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return LD(&r->seq) == s;
}

void irq_late_note(const struct irq_late_in *in)
{
    int cpu = this_cpu()->id;
    if (cpu < 0 || cpu >= MAX_CPUS)
        return;
    struct irql_state *t = &st[cpu];
    struct irql_req *r = &rq[cpu];
    uint32_t s = t->seq;         /* 單一寫入者:自己的 seq 不必 atomic 讀 */

    ST(&t->seq, s + 1);
    __atomic_thread_fence(__ATOMIC_RELEASE);

    ST(&t->heartbeat, t->heartbeat + 1);

    /* 套用新的設定請求:gen 換新、清 last_entry(下一次只初始化) */
    uint32_t pg = __atomic_load_n(&r->gen, __ATOMIC_ACQUIRE);
    if (pg != t->gen) {
        uint32_t g, cfg, hi, lo;
        if (req_read(r, &g, &cfg, &hi, &lo)) {
            ST(&t->gen, g);
            ST(&t->cfg, (cfg & 0xFFFFu) | ((hi | lo) ? 1u << 16 : 0u));
            ST(&t->iv_hi, hi);
            ST(&t->iv_lo, lo);
            ST(&t->last_hi, 0u);
            ST(&t->last_lo, 0u);
        }
    }

    uint32_t cfg = t->cfg;
    uint8_t kind = (uint8_t)(cfg >> 8);
    if ((cfg >> 16) & 1u) {
        uint64_t iv = mk64(t->iv_hi, t->iv_lo);
        uint64_t last = mk64(t->last_hi, t->last_lo);
        uint64_t entry = in->entry;
        uint32_t flags = in->flags & IRQL_F_RESYNC;
        uint64_t ref = 0;
        int rec = 0, below = 0;
        int match = in->kind == kind && in->src_id == (uint8_t)cfg;
        if (!match) {
            /* 丟棄也必須留證據:不保證每次來源不符都伴隨設定 gen 改變。 */
            ST(&t->rejected, t->rejected + 1);
        } else if (kind == IRQL_KIND_DEADLINE) {
            ref = in->ref;
            if (entry < ref)
                flags |= IRQL_F_EARLY;
            if (in->catchup_n >= IRQL_MIN_LATE_DEADLINE + 1u || flags)
                rec = 1;                     /* 異常(RESYNC / EARLY)不受門檻影響 */
            else if (in->catchup_n >= 2)
                below = 1;
        } else if (kind == IRQL_KIND_GAP) {
            ref = last;
            /* 間距 d = entry - ref;只在 entry >= ref 時比較 */
            if (last != 0 && entry >= ref) {
                uint64_t d = entry - ref;
                /* d >= IRQL_MIN_GAP_TICKS × iv:逐次減,不用乘法(不會溢位) */
                uint64_t left = d;
                unsigned k = 0;
                while (k < IRQL_MIN_GAP_TICKS && left >= iv) {
                    left -= iv;
                    k++;
                }
                if (k >= IRQL_MIN_GAP_TICKS)
                    rec = 1;
                else if (k >= 2)
                    below = 1;
            }
        }
        if (match && last != 0 && entry < last) {
            flags |= IRQL_F_BACKWARD;
            rec = 1;
            if (kind == IRQL_KIND_GAP)
                ref = last;
        }
        if (rec) {
            uint32_t n = t->total;
            struct irql_slot *sl = &t->ring[n & (IRQL_RING - 1)];
            uint16_t cn = kind == IRQL_KIND_DEADLINE ? in->catchup_n : 0;
            ST(&sl->rec_no, n);
            ST(&sl->meta, (uint32_t)kind | flags << 8 | (uint32_t)cn << 16);
            ST(&sl->ref_hi, (uint32_t)(ref >> 32));
            ST(&sl->ref_lo, (uint32_t)ref);
            ST(&sl->ent_hi, (uint32_t)(entry >> 32));
            ST(&sl->ent_lo, (uint32_t)entry);
            ST(&t->total, n + 1);
        } else if (below) {
            ST(&t->below_min, t->below_min + 1);
        }
        if (match) {
            ST(&t->last_hi, (uint32_t)(entry >> 32));
            ST(&t->last_lo, (uint32_t)entry);
        }
    }

    __atomic_thread_fence(__ATOMIC_RELEASE);
    ST(&t->seq, s + 2);
}

void irq_late_snapshot(int cpu, struct irq_late_snap *out)
{
    out->ok = 0;
    if (cpu < 0 || cpu >= MAX_CPUS)
        return;
    const struct irql_state *t = &st[cpu];
    for (int tries = 0; tries < IRQL_SNAP_TRIES; tries++) {
        uint32_t s = __atomic_load_n(&t->seq, __ATOMIC_ACQUIRE);
        if (s & 1) {
            arch_cpu_relax();
            continue;
        }
        uint32_t cfg = LD(&t->cfg);
        out->gen = LD(&t->gen);
        out->src_id = (uint8_t)cfg;
        out->kind = (uint8_t)(cfg >> 8);
        out->enabled = (uint8_t)((cfg >> 16) & 1u);
        out->iv = mk64(LD(&t->iv_hi), LD(&t->iv_lo));
        out->heartbeat = LD(&t->heartbeat);
        out->total = LD(&t->total);
        out->below_min = LD(&t->below_min);
        out->rejected = LD(&t->rejected);
        out->last_entry = mk64(LD(&t->last_hi), LD(&t->last_lo));
        for (uint32_t i = 0; i < IRQL_RING; i++) {
            const struct irql_slot *sl = &t->ring[i];
            uint32_t m = LD(&sl->meta);
            out->ring[i].rec_no = LD(&sl->rec_no);
            out->ring[i].kind = (uint8_t)m;
            out->ring[i].flags = (uint8_t)(m >> 8);
            out->ring[i].catchup_n = (uint16_t)(m >> 16);
            out->ring[i].ref = mk64(LD(&sl->ref_hi), LD(&sl->ref_lo));
            out->ring[i].entry = mk64(LD(&sl->ent_hi), LD(&sl->ent_lo));
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (LD(&t->seq) == s) {
            /* writer 取得 seq 後、發布 gen 前也屬 pending。前後 seq 不同時保守拒絕。
               此處是請求觀測點;在最後一次 seq 讀取後開始的 config 屬後續操作。 */
            uint32_t rs = __atomic_load_n(&rq[cpu].seq, __ATOMIC_ACQUIRE);
            uint32_t rg = __atomic_load_n(&rq[cpu].gen, __ATOMIC_ACQUIRE);
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            uint32_t re = LD(&rq[cpu].seq);
            out->pending = (rs & 1u) || rs != re || rg != out->gen;
            out->ok = 1;
            return;
        }
    }
}
