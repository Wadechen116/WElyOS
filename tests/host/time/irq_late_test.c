/*
 * irq_late_test.c - 在主機上測 include/irq_late.h 的純函式(I-351 第一階段)
 *   irq_late_ticks 的邊界、irq_late_check 的每個不完整原因、irq_late_classify 的重疊分類。
 * 不碰 kernel 狀態;每一項都有預期值斷言,最後印 PASS 與檢查數。
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../../include/irq_late.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
#define EQ(got, exp, what) do { unsigned long long g_ = (got), e_ = (exp); \
    CHECK(g_ == e_, "%s: got 0x%llx want 0x%llx", what, g_, e_); } while (0)

#define IV 100u

/* 一致、啟用的 DEADLINE 快照 */
static void base(struct irq_late_snap *s, uint32_t total, uint32_t hb, uint64_t last)
{
    memset(s, 0, sizeof *s);
    s->ok = 1; s->gen = 3; s->src_id = 1; s->kind = IRQL_KIND_DEADLINE; s->enabled = 1;
    s->iv = IV; s->heartbeat = hb; s->total = total; s->last_entry = last;
}

/* 寫入一筆紀錄到 B.ring(rec_no = no) */
static void put(struct irq_late_snap *s, uint32_t no, uint8_t kind, uint8_t flags, uint16_t cn,
                uint64_t ref, uint64_t entry)
{
    struct irq_late_rec *r = &s->ring[no & (IRQL_RING - 1)];
    r->rec_no = no; r->kind = kind; r->flags = flags; r->catchup_n = cn; r->ref = ref; r->entry = entry;
}

static void test_ticks(void)
{
    EQ(irq_late_ticks(0, 1000, 0), 0, "iv=0");
    EQ(irq_late_ticks(500, 500, IV), 0, "entry==ref");
    EQ(irq_late_ticks(500, 400, IV), 0, "entry<ref");
    EQ(irq_late_ticks(0, 99, IV), 0, "不到一個 iv");
    EQ(irq_late_ticks(0, 100, IV), 1, "剛好 1 倍");
    EQ(irq_late_ticks(0, 300, IV), 3, "剛好 3 倍");
    EQ(irq_late_ticks(0, 399, IV), 3, "3.99 倍取 floor");
    EQ(irq_late_ticks(0, (uint64_t)(IRQL_LATE_SAT - 1) * IV + 99, IV), IRQL_LATE_SAT - 1, "飽和前一格");
    EQ(irq_late_ticks(0, (uint64_t)IRQL_LATE_SAT * IV, IV), IRQL_LATE_SAT, "剛好飽和");
    EQ(irq_late_ticks(0, UINT64_MAX, 1), IRQL_LATE_SAT, "巨大差值飽和");
    EQ(irq_late_ticks(UINT64_MAX - 250, UINT64_MAX, IV), 2, "高位元不溢位");
}

static void test_check(void)
{
    struct irq_late_snap a, b;
    uint32_t f, n, inc;

    /* 完整:兩筆紀錄,一筆 late=1(catchup 2),一筆 late=4(catchup 5) */
    base(&a, 5, 10, 1000); base(&b, 7, 20, 5000);
    put(&b, 5, IRQL_KIND_DEADLINE, 0, 2, 2000, 2150);
    put(&b, 6, IRQL_KIND_DEADLINE, 0, 5, 3000, 3420);
    inc = irq_late_check(&a, &b, 4000, &f, &n);
    EQ(inc, 0, "完整"); EQ(f, 5, "完整 first"); EQ(n, 2, "完整 count");

    /* 沒有新紀錄也完整 */
    base(&a, 5, 10, 1000); base(&b, 5, 11, 5000);
    inc = irq_late_check(&a, &b, 4000, &f, &n);
    EQ(inc, 0, "零筆完整"); EQ(n, 0, "零筆 count");

    /* BUSY:任一份 ok=0,且只回 BUSY、first/count 為 0 */
    base(&a, 5, 10, 1000); base(&b, 7, 20, 5000); a.ok = 0;
    inc = irq_late_check(&a, &b, 4000, &f, &n);
    EQ(inc, IRQL_INC_BUSY, "A BUSY"); EQ(f, 0, "BUSY first"); EQ(n, 0, "BUSY count");
    a.ok = 1; b.ok = 0;
    EQ(irq_late_check(&a, &b, 4000, &f, &n), IRQL_INC_BUSY, "B BUSY");

    /* DISABLED */
    base(&a, 5, 10, 1000); base(&b, 5, 20, 5000); a.enabled = 0;
    EQ(irq_late_check(&a, &b, 4000, &f, &n), IRQL_INC_DISABLED, "A DISABLED");
    a.enabled = 1; b.enabled = 0;
    EQ(irq_late_check(&a, &b, 4000, &f, &n), IRQL_INC_DISABLED, "B DISABLED");

    /* GEN / src / kind 變化 */
    base(&a, 5, 10, 1000); base(&b, 5, 20, 5000); b.gen = 4;
    EQ(irq_late_check(&a, &b, 4000, &f, &n), IRQL_INC_GEN, "gen 變了");
    base(&b, 5, 20, 5000); b.src_id = 2;
    EQ(irq_late_check(&a, &b, 4000, &f, &n), IRQL_INC_GEN, "src 變了");
    base(&b, 5, 20, 5000); b.kind = IRQL_KIND_GAP;
    EQ(irq_late_check(&a, &b, 4000, &f, &n), IRQL_INC_GEN, "kind 變了");

    /* NO_HEARTBEAT */
    base(&a, 5, 10, 1000); base(&b, 5, 10, 5000);
    EQ(irq_late_check(&a, &b, 4000, &f, &n), IRQL_INC_NO_HEARTBEAT, "heartbeat 沒前進");

    /* NOT_PAST_C1:等於 c1 也不算超過 */
    base(&a, 5, 10, 1000); base(&b, 5, 20, 4000);
    EQ(irq_late_check(&a, &b, 4000, &f, &n), IRQL_INC_NOT_PAST_C1, "last_entry == c1");
    b.last_entry = 4001;
    EQ(irq_late_check(&a, &b, 4000, &f, &n), 0, "last_entry = c1+1 完整");

    /* OVERWRITTEN:剛好 RING 筆仍完整;RING+1 覆寫;超過半圈(B 落後 A) */
    base(&a, 5, 10, 1000); base(&b, 5 + IRQL_RING, 20, 5000);
    for (uint32_t i = 0; i < IRQL_RING; i++)
        put(&b, 5 + i, IRQL_KIND_DEADLINE, 0, 2, 2000, 2100);
    inc = irq_late_check(&a, &b, 4000, &f, &n);
    EQ(inc, 0, "剛好 RING 筆"); EQ(n, IRQL_RING, "RING count");
    b.total = 5 + IRQL_RING + 1;
    put(&b, 5 + IRQL_RING, IRQL_KIND_DEADLINE, 0, 2, 2000, 2100);
    inc = irq_late_check(&a, &b, 4000, &f, &n);
    EQ(inc, IRQL_INC_OVERWRITTEN, "RING+1 筆"); EQ(n, 0, "覆寫 count");
    base(&b, 4, 20, 5000);       /* B.total < A.total → 差值 0xFFFFFFFF,超過半圈 */
    EQ(irq_late_check(&a, &b, 4000, &f, &n), IRQL_INC_OVERWRITTEN, "B 落後 A(超過半圈)");
    base(&b, 5 + 0x80000001u, 20, 5000);
    EQ(irq_late_check(&a, &b, 4000, &f, &n), IRQL_INC_OVERWRITTEN, "差值 0x80000001");

    /* total 32 位元 wrap:A=0xFFFFFFFE,B=2,四筆跨 0xFFFFFFFF→0 */
    base(&a, 0xFFFFFFFEu, 10, 1000); base(&b, 2, 20, 5000);
    put(&b, 0xFFFFFFFEu, IRQL_KIND_DEADLINE, 0, 2, 2000, 2100);
    put(&b, 0xFFFFFFFFu, IRQL_KIND_DEADLINE, 0, 2, 2000, 2100);
    put(&b, 0, IRQL_KIND_DEADLINE, 0, 2, 2000, 2100);
    put(&b, 1, IRQL_KIND_DEADLINE, 0, 2, 2000, 2100);
    inc = irq_late_check(&a, &b, 4000, &f, &n);
    EQ(inc, 0, "wrap 完整"); EQ(f, 0xFFFFFFFEu, "wrap first"); EQ(n, 4, "wrap count");

    /* MISSING:slot 的 rec_no 不符(被更舊一圈的紀錄佔著) */
    base(&a, 5, 10, 1000); base(&b, 7, 20, 5000);
    put(&b, 5, IRQL_KIND_DEADLINE, 0, 2, 2000, 2100);
    put(&b, 6 - IRQL_RING, IRQL_KIND_DEADLINE, 0, 2, 2000, 2100);
    EQ(irq_late_check(&a, &b, 4000, &f, &n), IRQL_INC_MISSING, "rec_no 不符");

    /* INCONSISTENT:catchup_n != late+1 */
    base(&a, 5, 10, 1000); base(&b, 6, 20, 5000);
    put(&b, 5, IRQL_KIND_DEADLINE, 0, 3, 2000, 2150);   /* late=1,應為 2 */
    EQ(irq_late_check(&a, &b, 4000, &f, &n), IRQL_INC_INCONSISTENT, "catchup_n != late+1");
    /* RESYNC / EARLY 不套用 catchup 等式；BACKWARD 必須標示不完整。 */
    put(&b, 5, IRQL_KIND_DEADLINE, IRQL_F_RESYNC, 3, 2000, 2150);
    EQ(irq_late_check(&a, &b, 4000, &f, &n), 0, "RESYNC 免檢");
    put(&b, 5, IRQL_KIND_DEADLINE, IRQL_F_EARLY, 1, 2000, 1990);
    EQ(irq_late_check(&a, &b, 4000, &f, &n), 0, "EARLY 免檢");
    put(&b, 5, IRQL_KIND_DEADLINE, IRQL_F_BACKWARD, 9, 2000, 2150);
    EQ(irq_late_check(&a, &b, 4000, &f, &n), IRQL_INC_INCONSISTENT, "BACKWARD 不完整");
    a.kind = b.kind = IRQL_KIND_GAP;   /* GAP 必須與快照種類一致，且不套用 catchup 等式。 */
    put(&b, 5, IRQL_KIND_GAP, 0, 0, 2000, 2350);
    EQ(irq_late_check(&a, &b, 4000, &f, &n), 0, "GAP 免檢");

    /* 多個原因同時 */
    base(&a, 5, 10, 1000); base(&b, 5, 10, 3000); b.gen = 9;
    EQ(irq_late_check(&a, &b, 4000, &f, &n),
       IRQL_INC_GEN | IRQL_INC_NO_HEARTBEAT | IRQL_INC_NOT_PAST_C1, "多原因");
}

static void test_classify(void)
{
    struct irq_late_snap b;
    base(&b, 0, 20, 99999);
    /* 樣本 [1000, 2000] */
    put(&b, 0, IRQL_KIND_DEADLINE, 0, 2, 100, 200);       /* 無重疊 */
    EQ(irq_late_classify(&b, 0, 0, 1, 1000, 2000, 1, 5000, 6000), IRQL_REL_NO_RECORDED_EVENT_OVERLAP, "無重疊");
    EQ(irq_late_classify(&b, 0, 0, 0, 1000, 2000, 1, 5000, 6000), IRQL_REL_NO_RECORDED_EVENT_OVERLAP, "零筆");

    /* 端點相接:紀錄 [800,1000] 與樣本 [1000,2000];與注入 [0,800] 相接 */
    put(&b, 0, IRQL_KIND_DEADLINE, 0, 2, 800, 1000);
    EQ(irq_late_classify(&b, 0, 0, 1, 1000, 2000, 1, 5000, 6000), IRQL_REL_OUTSIDE_INJECTION, "樣本端點相接");
    EQ(irq_late_classify(&b, 0, 0, 1, 1000, 2000, 1, 0, 800), IRQL_REL_INJECTION_OVERLAP, "注入端點相接");
    EQ(irq_late_classify(&b, 0, 0, 1, 1001, 2000, 1, 5000, 6000), IRQL_REL_NO_RECORDED_EVENT_OVERLAP, "差 1 不相接");

    /* 只注入內 / 只注入外 / has_inj=0 */
    put(&b, 0, IRQL_KIND_DEADLINE, 0, 2, 1500, 1700);
    EQ(irq_late_classify(&b, 0, 0, 1, 1000, 2000, 1, 1600, 1650), IRQL_REL_INJECTION_OVERLAP, "只注入內");
    EQ(irq_late_classify(&b, 0, 0, 1, 1000, 2000, 1, 1800, 1900), IRQL_REL_OUTSIDE_INJECTION, "只注入外");
    EQ(irq_late_classify(&b, 0, 0, 1, 1000, 2000, 0, 1600, 1650), IRQL_REL_OUTSIDE_INJECTION, "has_inj=0");

    /* 多筆混合 → AMBIGUOUS(GAP + DEADLINE 都參與),含一筆不重疊的 */
    put(&b, 0, IRQL_KIND_DEADLINE, 0, 2, 1500, 1700);     /* 注入內 */
    put(&b, 1, IRQL_KIND_GAP, 0, 0, 1100, 1300);          /* 注入外 */
    put(&b, 2, IRQL_KIND_DEADLINE, 0, 2, 3000, 3100);     /* 不重疊 */
    EQ(irq_late_classify(&b, 0, 0, 3, 1000, 2000, 1, 1600, 1650), IRQL_REL_AMBIGUOUS, "多筆混合");
    EQ(irq_late_classify(&b, 0, 1, 2, 1000, 2000, 1, 1600, 1650), IRQL_REL_OUTSIDE_INJECTION, "只看 first 起");

    /* EARLY:ref > entry,區間退化為 [entry, entry] */
    put(&b, 0, IRQL_KIND_DEADLINE, IRQL_F_EARLY, 1, 3000, 1500);
    EQ(irq_late_classify(&b, 0, 0, 1, 1000, 2000, 1, 1500, 1500), IRQL_REL_INJECTION_OVERLAP, "EARLY 點在注入");
    EQ(irq_late_classify(&b, 0, 0, 1, 1000, 2000, 1, 1600, 2500), IRQL_REL_OUTSIDE_INJECTION, "EARLY 不延伸到 ref");
    EQ(irq_late_classify(&b, 0, 0, 1, 1600, 2000, 1, 5000, 6000), IRQL_REL_NO_RECORDED_EVENT_OVERLAP, "EARLY 點在樣本外");

    /* 證據不完整 */
    EQ(irq_late_classify(&b, IRQL_INC_MISSING, 0, 1, 1000, 2000, 1, 1500, 1500),
       IRQL_REL_EVIDENCE_INCOMPLETE, "inc!=0");
    EQ(irq_late_classify(&b, IRQL_INC_BUSY, 0, 0, 1000, 2000, 0, 0, 0), IRQL_REL_EVIDENCE_INCOMPLETE, "BUSY");
    CHECK(strcmp(irq_late_rel_name(IRQL_REL_AMBIGUOUS), "AMBIGUOUS") == 0, "rel_name");
}

int main(void)
{
    test_ticks();
    test_check();
    test_classify();
    if (fails) {
        printf("FAIL: %d / %d 項檢查失敗\n", fails, checks);
        return 1;
    }
    printf("PASS: irq_late 主機測試 %d 項檢查\n", checks);
    return 0;
}
