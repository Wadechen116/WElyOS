/*
 * I-351 獨立完整性審查探針：直接使用正式標頭，不複製分析器。
 * 每個檢查都要求缺陷已修；逐項輸出而不中途停止，方便修補後作為 gate。
 * gcc -std=c11 -Wall -Wextra -Werror -Iinclude \
 *   tests/host/time/probe_irq_late_integrity_review.c -o /tmp/irq-late-review
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "irq_late.h"

static unsigned checks, failures;

static void expect(const char *name, int condition)
{
    checks++;
    if (!condition)
        failures++;
    printf("%s %s\n", condition ? "PASS" : "FAIL", name);
}

/* c1=200；B 已觀察到 300 的中斷，且 A/B 是同一來源與尺度。 */
static void baseline(struct irq_late_snap *a, struct irq_late_snap *b)
{
    memset(a, 0, sizeof(*a));
    a->ok = 1;
    a->gen = 1;
    a->src_id = 1;
    a->kind = IRQL_KIND_GAP;
    a->enabled = 1;
    a->iv = 10;
    a->heartbeat = 10;
    a->last_entry = 100;
    *b = *a;
    b->heartbeat = 20;
    b->last_entry = 300;
}

static uint32_t check(const struct irq_late_snap *a, const struct irq_late_snap *b)
{
    uint32_t first, count;
    return irq_late_check(a, b, 200, &first, &count);
}

int main(void)
{
    struct irq_late_snap a, b;
    uint32_t first, count;

    baseline(&a, &b);
    expect("baseline_no_events_complete", check(&a, &b) == 0);
    expect("baseline_no_events_relation",
           irq_late_classify(&b, 0, 0, 0, 100, 200, 0, 0, 0) ==
           IRQL_REL_NO_RECORDED_EVENT_OVERLAP);

    /* A/B 即使 gen 相同，尚未套用的設定請求也使此觀測跨越來源切換。 */
    a.pending = 1;
    expect("start_snapshot_pending_rejected_as_generation",
           (check(&a, &b) & IRQL_INC_GEN) != 0);
    a.pending = 0;
    b.pending = 1;
    expect("end_snapshot_pending_rejected_as_generation",
           (check(&a, &b) & IRQL_INC_GEN) != 0);
    b.pending = 0;
    expect("both_snapshots_not_pending_complete", check(&a, &b) == 0);

    b.total = 1;
    b.ring[0].rec_no = 0;
    b.ring[0].kind = IRQL_KIND_GAP;
    b.ring[0].ref = 120;
    b.ring[0].entry = 160;
    uint32_t inc = irq_late_check(&a, &b, 200, &first, &count);
    expect("baseline_valid_gap_complete", inc == 0 && count == 1);
    expect("baseline_valid_gap_overlap",
           irq_late_classify(&b, inc, first, count, 100, 200, 1, 130, 150) ==
           IRQL_REL_INJECTION_OVERLAP);

    b.ring[0].flags = IRQL_F_BACKWARD;
    b.ring[0].ref = 160;
    b.ring[0].entry = 120;
    expect("backward_rejected", check(&a, &b) != 0);

    baseline(&a, &b);
    b.iv = 11;
    expect("changed_scale_rejected", check(&a, &b) != 0);

    baseline(&a, &b);
    a.iv = b.iv = 0;
    expect("enabled_zero_scale_rejected", check(&a, &b) != 0);

    baseline(&a, &b);
    a.kind = b.kind = UINT8_MAX;
    expect("invalid_snapshot_kind_rejected", check(&a, &b) != 0);

    baseline(&a, &b);
    b.total = 1;
    b.ring[0].kind = UINT8_MAX;
    expect("invalid_record_kind_rejected", check(&a, &b) != 0);

    baseline(&a, &b);
    b.total = 1;  /* 模擬 total 前進，但第一筆零初始化 slot 沒有寫入。 */
    expect("first_unwritten_slot_rejected", check(&a, &b) != 0);

    baseline(&a, &b);
    /* 只走 RING+1 次；原分析器用 mask 索引，因此不會讀出 ring 邊界。 */
    expect("count_exceeding_ring_rejected",
           irq_late_classify(&b, 0, 0, IRQL_RING + 1u, 100, 200, 0, 0, 0) ==
           IRQL_REL_EVIDENCE_INCOMPLETE);
    expect("reversed_sample_rejected",
           irq_late_classify(&b, 0, 0, 0, 200, 100, 0, 0, 0) ==
           IRQL_REL_EVIDENCE_INCOMPLETE);
    expect("reversed_injection_rejected",
           irq_late_classify(&b, 0, 0, 0, 100, 200, 1, 160, 120) ==
           IRQL_REL_EVIDENCE_INCOMPLETE);

    baseline(&a, &b);
    a.kind = b.kind = IRQL_KIND_DEADLINE;
    b.total = 1;
    b.ring[0].kind = IRQL_KIND_DEADLINE;
    b.ring[0].flags = IRQL_F_EARLY;
    b.ring[0].ref = 160;
    b.ring[0].entry = 120;
    b.ring[0].catchup_n = 0;
    inc = irq_late_check(&a, &b, 200, &first, &count);
    expect("legal_early_complete", inc == 0 && count == 1);
    expect("legal_early_point_overlap",
           irq_late_classify(&b, inc, first, count, 100, 200, 1, 110, 130) ==
           IRQL_REL_INJECTION_OVERLAP);

    /* floor((3090-100)/10)=299，catchup_n=300；顯示用 ticks 飽和不能破壞完整性。 */
    b.ring[0].flags = 0;
    b.ring[0].ref = 100;
    b.ring[0].entry = 3090;
    b.ring[0].catchup_n = 300;
    b.last_entry = 3100;
    expect("large_catchup_ticks_saturated",
           irq_late_ticks(100, 3090, 10) == IRQL_LATE_SAT);
    inc = irq_late_check(&a, &b, 200, &first, &count);
    expect("legal_large_catchup_without_resync_complete", inc == 0 && count == 1);

    baseline(&a, &b);
    b.total = 1;
    b.ring[0].kind = IRQL_KIND_GAP;
    b.ring[0].ref = 120;
    b.ring[0].entry = 160;
    b.ring[0].flags = 0x80u;
    expect("unknown_record_flags_rejected", check(&a, &b) != 0);

    b.ring[0].flags = 0;
    b.ring[0].kind = IRQL_KIND_DEADLINE;
    b.ring[0].catchup_n = 5;  /* 單筆本身合法，但和 snapshot 的 GAP 種類不符。 */
    expect("record_snapshot_kind_mismatch_rejected", check(&a, &b) != 0);

    baseline(&a, &b);
    b.total = IRQL_RING;
    for (uint32_t i = 0; i < IRQL_RING; ++i) {
        b.ring[i].rec_no = i;
        b.ring[i].kind = IRQL_KIND_GAP;
        b.ring[i].ref = 120 + i;
        b.ring[i].entry = 160 + i;
    }
    inc = irq_late_check(&a, &b, 200, &first, &count);
    expect("exact_ring_capacity_complete", inc == 0 && first == 0 && count == IRQL_RING);
    b.total = IRQL_RING + 1u;
    expect("one_beyond_ring_capacity_overwritten",
           (check(&a, &b) & IRQL_INC_OVERWRITTEN) != 0);

    printf("irq-late integrity review: %u/%u passed, %u failed\n",
           checks - failures, checks, failures);
    return failures ? 1 : 0;
}
