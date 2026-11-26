#ifndef WELYOS_IRQ_LATE_H
#define WELYOS_IRQ_LATE_H
/*
 * irq_late:每顆 CPU 計時器中斷「晚到」的獨立觀測(I-351 第一階段,Claude 265 / Codex 046、047)。
 *
 * 只觀測、只印:不改 timer_note_catchup 的補記統計、不改排程與系統時間推進、不改任何測試的通過條件。
 * 不做因果歸因 — 輸出只描述「時間區間重疊關係」與「證據是否完整」。
 *
 * 兩種紀錄(不可直接比較、不合併統計):
 *   IRQL_KIND_DEADLINE:有硬體預定到期時刻的計時器(riscv64、aarch64、i386 第 0 顆的 TSC 路徑)。
 *       ref = 補記迴圈之前的預定 deadline;catchup_n = 本次補記的 tick 數;RESYNC 由本次補記結果帶入。
 *   IRQL_KIND_GAP:i386 其他 CPU 的 LAPIC 週期模式,讀不到預定 deadline。ref = 上一次中斷入口時刻。
 *       只是原始的「兩次入口間距」,不是 lateness 的下界。
 * ref、entry 都是同一個共用計數器的計數(src_id 標明哪一個);跨 gen 或 src_id 一律不比較。
 *
 * 本檔分兩部分:(1) 型別與 kernel 端 API 宣告;(2) 純函式分析器(static inline,主機端測試可直接 include,
 * 不碰任何 kernel 狀態)。沒有除法(i386 不可用 64 位元除法),迴圈都有上限。
 */
#include <stdint.h>

#define IRQL_RING        32u     /* 每顆 CPU 的環形紀錄筆數(負對照副本可改小;Codex 072) */
/*
 * 記錄門檻(Codex 072 同意的「容量 / 觀測篩選候選」,不是逾時可靠度門檻的校準):
 * 依第 76 步的分布量測(build/i351/r256/),none 時 riscv64 / aarch64 全部晚 1 tick、i386 間距全部 2 tick,
 * 所以門檻設在這之上。低於門檻的晚到只計數(below_min),不寫紀錄、不當證據;「完整」只表示「門檻以上
 * 的紀錄完整」,門檻以下的小事件仍可能影響逾時。RESYNC / EARLY / BACKWARD 等異常不受門檻影響,一律記錄。
 */
#define IRQL_MIN_LATE_DEADLINE 2u   /* DEADLINE:late_ticks >= 2(即 catchup_n >= 3)才記 */
#define IRQL_MIN_GAP_TICKS     3u   /* GAP:入口間距 >= 3 × iv 才記 */
#define IRQL_LATE_SAT    255u    /* late_ticks 的飽和值 */
#define IRQL_SNAP_TRIES  1000    /* snapshot 的有界重試次數 */

_Static_assert(IRQL_RING != 0 && (IRQL_RING & (IRQL_RING - 1u)) == 0,
               "IRQL_RING 必須是非零的 2 次冪");

enum irq_late_kind { IRQL_KIND_NONE = 0, IRQL_KIND_DEADLINE = 1, IRQL_KIND_GAP = 2 };

/* 紀錄旗標 */
enum {
    IRQL_F_RESYNC   = 1u << 0,   /* 本次落後太多、放棄補記重新對時(由 arch 補記結果帶入) */
    IRQL_F_EARLY    = 1u << 1,   /* entry < ref:有效的提前送達,不是異常 */
    IRQL_F_BACKWARD = 1u << 2,   /* entry < 上一次 entry:計數器倒退 */
};

/* arch 計時器中斷交給 irq_late_note 的輸入(補記迴圈之後呼叫) */
struct irq_late_in {
    uint8_t  kind;               /* enum irq_late_kind,必須等於 config 的 kind */
    uint8_t  flags;              /* 只用 IRQL_F_RESYNC;EARLY / BACKWARD 由 irq_late_note 判斷 */
    uint16_t catchup_n;          /* DEADLINE:本次補記數(timer_note_catchup 的 n);GAP:0 */
    uint64_t ref;                /* DEADLINE:補記前的預定 deadline;GAP:忽略(由模組用上一次 entry) */
    uint64_t entry;              /* 中斷入口讀到的共用計數器值 */
    uint8_t  src_id;             /* 讀 entry 當下的來源 ID;和已套用的設定不同就不記(來源切換中,尺度不可混用;Codex 051) */
};
#define IRQL_SRC_UNKNOWN 0xFFu   /* 讀計數器前後來源不同(切換中):這次一定不記 */

/* 一筆紀錄(只在符合記錄條件時寫入) */
struct irq_late_rec {
    uint32_t rec_no;             /* 寫入時的 total(從 0 起);讀者用來偵測遺失 / 覆寫 */
    uint8_t  kind;
    uint8_t  flags;
    uint16_t catchup_n;
    uint64_t ref;
    uint64_t entry;
};

/* 一顆 CPU 的一致快照(irq_late_snapshot 的輸出) */
struct irq_late_snap {
    int      ok;                 /* 1 = 在 IRQL_SNAP_TRIES 內取得一致的一份;0 = BUSY */
    uint32_t gen;                /* 每次 config +1(已套用的) */
    uint8_t  pending;            /* 取快照時有尚未套用的設定請求(req_gen != gen 或 request 寫入中/變動) */
    uint8_t  src_id;             /* 計數器來源 ID(arch 定義,例如 i386 tsc/hpet/pm) */
    uint8_t  kind;
    uint8_t  enabled;            /* config 時 iv != 0 */
    uint64_t iv;                 /* 每 tick 的計數 */
    uint32_t heartbeat;          /* 每次計時器中斷 +1(不論是否寫紀錄) */
    uint32_t total;              /* 已寫入的紀錄數 */
    uint32_t rejected;           /* 啟用期間來源或種類不符的累計丟棄數 */
    uint32_t below_min;          /* 有晚到、但低於記錄門檻而沒寫紀錄的次數(只計數,不當證據) */
    uint64_t last_entry;         /* 最後一次中斷入口(0 = 尚未有) */
    struct irq_late_rec ring[IRQL_RING];   /* ring[rec_no % IRQL_RING];IRQL_RING 須為 2 的冪 */
};

/* ---- kernel 端 API(kernel/irq_late.c)---- */
/* 開機與每次來源切換 / 降級時呼叫(可在任何 CPU 上為任一顆設定;寫入以 seqlock 發布)。gen++。 */
void irq_late_config(int cpu, uint8_t src_id, uint8_t kind, uint64_t iv);
/* 本顆 CPU 的計時器中斷內呼叫(只有本顆寫自己的狀態)。不配置、不印、不除法;唯一的迴圈是 GAP 門檻比較,最多 IRQL_MIN_GAP_TICKS 次。
   記錄條件:DEADLINE:catchup_n >= IRQL_MIN_LATE_DEADLINE + 1 或 RESYNC 或 entry < ref;
   GAP:有上一次 entry 且 entry - ref >= IRQL_MIN_GAP_TICKS × iv。
   entry < 上一次 entry 時記錄並標 BACKWARD。未啟用時只遞增 heartbeat。 */
void irq_late_note(const struct irq_late_in *in);
/* 有界重試取得一致快照;失敗時 out->ok = 0。 */
void irq_late_snapshot(int cpu, struct irq_late_snap *out);

/* ---- 純函式分析器 ---- */

/* 完整性結果(位元,可同時多個;0 = 完整) */
enum {
    IRQL_INC_BUSY         = 1u << 0,   /* 任一份 snapshot 不一致 */
    IRQL_INC_DISABLED     = 1u << 1,   /* A 或 B 未啟用 */
    IRQL_INC_GEN          = 1u << 2,   /* gen / src_id / kind 在 A、B 之間變了 */
    IRQL_INC_NO_HEARTBEAT = 1u << 3,   /* heartbeat 沒前進(CPU 離線或不計時) */
    IRQL_INC_OVERWRITTEN  = 1u << 4,   /* B.total - A.total > IRQL_RING,或差值超過半圈無法證明 */
    IRQL_INC_MISSING      = 1u << 5,   /* 預期的 slot 的 rec_no 不符 */
    IRQL_INC_NOT_PAST_C1  = 1u << 6,   /* B.last_entry 沒有超過觀測終點 c1 */
    IRQL_INC_REJECTED     = 1u << 8,   /* A/B 間有輸入因來源或種類不符而丟棄 */
    IRQL_INC_INCONSISTENT = 1u << 7,   /* DEADLINE 非 RESYNC 紀錄 catchup_n != late_ticks + 1 */
};

/* floor((entry - ref) / iv),entry <= ref 時 0;有界比較迴圈,飽和 IRQL_LATE_SAT。iv == 0 回傳 0。 */
static inline uint32_t irq_late_ticks(uint64_t ref, uint64_t entry, uint64_t iv)
{
    if (iv == 0 || entry <= ref)
        return 0;
    uint64_t d = entry - ref;
    uint32_t k = 0;
    while (d >= iv && k < IRQL_LATE_SAT) {
        d -= iv;
        k++;
    }
    return k;
}

/* 零初始化 slot 的 kind=NONE 也必須拒絕，不能只靠 rec_no 判定已寫入。 */
static inline int irq_late_record_valid(const struct irq_late_rec *r, uint8_t kind)
{
    return (kind == IRQL_KIND_DEADLINE || kind == IRQL_KIND_GAP) &&
           r->kind == kind &&
           !(r->flags & ~(IRQL_F_RESYNC | IRQL_F_EARLY | IRQL_F_BACKWARD)) &&
           !(r->flags & IRQL_F_BACKWARD);
}

/*
 * 檢查 A(觀測開始前)到 B(觀測結束後)之間的紀錄是否完整,回傳 IRQL_INC_* 位元。
 * 前置條件(呼叫端負責,本函式不驗證、也無從驗證):A 必須在觀測起點 c0 之前取得;本函式只檢查終點(B.last_entry > c1)。
 * 完整時 *first = A.total,*count = B.total - A.total(這些 rec_no 的紀錄都在 B.ring 裡)。
 */
static inline uint32_t irq_late_check(const struct irq_late_snap *a, const struct irq_late_snap *b,
                                      uint64_t c1, uint32_t *first, uint32_t *count)
{
    uint32_t inc = 0;
    *first = 0;
    *count = 0;
    if (!a->ok || !b->ok)
        return IRQL_INC_BUSY;
    if (!a->enabled || !b->enabled)
        inc |= IRQL_INC_DISABLED;
    if (a->gen != b->gen || a->src_id != b->src_id || a->kind != b->kind || a->iv != b->iv ||
        a->pending || b->pending)       /* 切換已請求、尚未套用:不可判完整(Codex 052) */
        inc |= IRQL_INC_GEN;
    if (!a->iv || !b->iv ||
        (a->kind != IRQL_KIND_DEADLINE && a->kind != IRQL_KIND_GAP) ||
        (b->kind != IRQL_KIND_DEADLINE && b->kind != IRQL_KIND_GAP))
        inc |= IRQL_INC_INCONSISTENT;
    if (b->rejected != a->rejected)
        inc |= IRQL_INC_REJECTED;
    if (b->heartbeat == a->heartbeat)
        inc |= IRQL_INC_NO_HEARTBEAT;
    if (b->last_entry <= c1)
        inc |= IRQL_INC_NOT_PAST_C1;
    uint32_t n = b->total - a->total;              /* 32 位元模減 */
    if (n > 0x80000000u || n > IRQL_RING) {
        inc |= IRQL_INC_OVERWRITTEN;
        return inc;
    }
    for (uint32_t i = 0; i < n; i++) {
        uint32_t no = a->total + i;
        const struct irq_late_rec *r = &b->ring[no & (IRQL_RING - 1)];
        if (r->rec_no != no) {
            inc |= IRQL_INC_MISSING;
            continue;
        }
        if (!irq_late_record_valid(r, b->kind)) {
            inc |= IRQL_INC_INCONSISTENT;
            continue;
        }
        if (r->kind == IRQL_KIND_DEADLINE && !(r->flags & (IRQL_F_RESYNC | IRQL_F_EARLY))) {
            uint32_t ticks = irq_late_ticks(r->ref, r->entry, b->iv);
            /* 飽和只表示至少 255 tick，不得拿它要求精確的 catchup_n。 */
            if (ticks == IRQL_LATE_SAT ? r->catchup_n < IRQL_LATE_SAT + 1u :
                (uint32_t)r->catchup_n != ticks + 1u)
                inc |= IRQL_INC_INCONSISTENT;
        }
    }
    *first = a->total;
    *count = n;
    return inc;
}

/* 一個超時樣本相對於紀錄的時間重疊關係(只描述重疊,不描述因果) */
enum irq_late_rel {
    IRQL_REL_NO_RECORDED_EVENT_OVERLAP = 0,   /* 門檻以上的紀錄完整,且沒有任何紀錄區間與樣本重疊
                                                 (門檻以下的小事件沒有紀錄,不能排除) */
    IRQL_REL_INJECTION_OVERLAP,            /* 重疊的紀錄全部與注入區間有交集 */
    IRQL_REL_OUTSIDE_INJECTION,            /* 重疊的紀錄全部與注入區間無交集(含沒有注入) */
    IRQL_REL_AMBIGUOUS,                    /* 重疊的紀錄兩種都有 */
    IRQL_REL_EVIDENCE_INCOMPLETE,          /* irq_late_check 不完整 */
    IRQL_REL_SAMPLES_INCOMPLETE,           /* 超時樣本被截斷、沒有保存區間 */
};

/* 閉區間 [a0, a1] 與 [b0, b1] 是否有交集(端點相接算交集) */
static inline int irq_late_overlap(uint64_t a0, uint64_t a1, uint64_t b0, uint64_t b1)
{
    return a0 <= b1 && b0 <= a1;
}

/*
 * 分類一個超時樣本 [t0, t1]。inc = irq_late_check 的結果;紀錄取 B.ring 中 rec_no 為 first..first+count-1 者;
 * 每筆紀錄的區間是 [min(ref, entry), entry](EARLY 時 ref > entry,區間退化成 [entry, entry])。
 * has_inj = 0 表示沒有注入(此時重疊的紀錄一律算 OUTSIDE_INJECTION)。GAP 與 DEADLINE 紀錄都參與,
 * 呼叫端另外印出種類,不合併統計。多筆重疊紀錄綜合判斷,不因其中一筆而忽略其他筆。
 */
static inline enum irq_late_rel irq_late_classify(const struct irq_late_snap *b, uint32_t inc,
                                                  uint32_t first, uint32_t count,
                                                  uint64_t t0, uint64_t t1,
                                                  int has_inj, uint64_t s0, uint64_t s1)
{
    if (inc || count > IRQL_RING || t0 > t1 || (has_inj && s0 > s1) ||
        !b->iv || (b->kind != IRQL_KIND_DEADLINE && b->kind != IRQL_KIND_GAP))
        return IRQL_REL_EVIDENCE_INCOMPLETE;
    int in_inj = 0, out_inj = 0;
    for (uint32_t i = 0; i < count; i++) {
        const struct irq_late_rec *r = &b->ring[(first + i) & (IRQL_RING - 1)];
        /* 種類與 snapshot 一致性由 check 負責；此函式也可單獨比較不同種類的合法區間。 */
        if (r->rec_no != first + i || !irq_late_record_valid(r, r->kind))
            return IRQL_REL_EVIDENCE_INCOMPLETE;
        uint64_t r0 = r->ref < r->entry ? r->ref : r->entry;
        if (!irq_late_overlap(r0, r->entry, t0, t1))
            continue;
        if (has_inj && irq_late_overlap(r0, r->entry, s0, s1))
            in_inj = 1;
        else
            out_inj = 1;
    }
    if (in_inj && out_inj)
        return IRQL_REL_AMBIGUOUS;
    if (in_inj)
        return IRQL_REL_INJECTION_OVERLAP;
    if (out_inj)
        return IRQL_REL_OUTSIDE_INJECTION;
    return IRQL_REL_NO_RECORDED_EVENT_OVERLAP;
}

static inline const char *irq_late_rel_name(enum irq_late_rel r)
{
    switch (r) {
    case IRQL_REL_NO_RECORDED_EVENT_OVERLAP: return "NO_RECORDED_EVENT_OVERLAP";
    case IRQL_REL_INJECTION_OVERLAP:      return "INJECTION_OVERLAP";
    case IRQL_REL_OUTSIDE_INJECTION:      return "OUTSIDE_INJECTION";
    case IRQL_REL_AMBIGUOUS:              return "AMBIGUOUS";
    case IRQL_REL_EVIDENCE_INCOMPLETE:    return "EVIDENCE_INCOMPLETE";
    case IRQL_REL_SAMPLES_INCOMPLETE:     return "SAMPLES_INCOMPLETE";
    }
    return "?";
}

#endif
