/* gcc -std=c11 -Wall -Wextra -Werror -Iinclude tests/host/time/lockhold_model_test.c */
#include <stdio.h>
#include <stdlib.h>
#include "lockhold_model.h"
static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static struct lh_stamp stamp(uint64_t n)
{
    return (struct lh_stamp){n, 1, 7, 1};
}
static void start(struct lh_model *m)
{
    *m = (struct lh_model){0};
    CHECK(lh_begin(m, 1, stamp(0)));
}
static void pair(struct lh_model *m, unsigned id, uint64_t a, uint64_t b)
{
    lh_acquire(m, id, stamp(a));
    lh_release(m, id, stamp(b));
}
static void normal(void)
{
    struct lh_model m;
    start(&m);
    lh_acquire(&m, LH_TIME, stamp(10));
    lh_acquire(&m, LH_SCHED, stamp(20));
    lh_release(&m, LH_SCHED, stamp(35));
    lh_release(&m, LH_TIME, stamp(50));
    CHECK(m.stat[0].count == 1 && m.stat[0].sum == 15 && m.stat[0].max == 15);
    CHECK(m.stat[1].count == 1 && m.stat[1].sum == 40 && m.stat[1].max == 40);
    pair(&m, LH_SCHED, 60, 60);
    CHECK(m.stat[0].count == 2 && m.stat[0].max == 15 && m.stat[0].sum == 15);
    CHECK(!lh_active(&m) && !m.flags);
    CHECK(lh_stop(&m, stamp(0)) && m.phase == LH_FROZEN);
    pair(&m, LH_SCHED, 100, 200);
    CHECK(m.stat[0].count == 2);
    CHECK(!lh_begin(&m, 1, stamp(0)));
    CHECK(lh_begin(&m, 2, stamp(0)) && m.stat[0].count == 0 && !m.flags);
}
static void lifecycle(void)
{
    struct lh_model m;
    start(&m);
    lh_acquire(&m, LH_SCHED, stamp(10));
    CHECK(!lh_stop(&m, stamp(0)) && m.phase == LH_DRAINING && lh_active(&m));
    CHECK(m.flags & LH_INCONSISTENT);
    CHECK(!lh_begin(&m, 2, stamp(0)) && lh_active(&m) && m.session == 1);
    lh_acquire(&m, LH_TIME, stamp(20));
    CHECK(!m.token[1].active);
    lh_release(&m, LH_SCHED, stamp(30));
    CHECK(!lh_active(&m) && m.stat[0].count == 1 && m.stat[0].sum == 20);
    CHECK(!lh_begin(&m, 2, stamp(0))); /* drain ACK 尚未完成 */
    CHECK(lh_stop(&m, stamp(0)) && m.phase == LH_FROZEN);
    CHECK(lh_begin(&m, 2, stamp(0)));
    lh_acquire(&m, LH_SCHED, stamp(100));
    lh_acquire(&m, LH_SCHED, stamp(200));
    CHECK(m.token[0].start.count == 100 && m.phase == LH_DRAINING);
    lh_release(&m, LH_SCHED, stamp(300));
    CHECK(m.stat[0].sum == 200);
    start(&m);
    lh_release(&m, LH_TIME, stamp(10));
    CHECK(m.flags & LH_INCONSISTENT);
    start(&m);
    lh_acquire(&m, 0, stamp(10));
    CHECK(m.flags & LH_INCONSISTENT);
    start(&m);
    lh_release(&m, 3, stamp(10));
    CHECK(m.flags & LH_INCONSISTENT);
    m = (struct lh_model){0};
    CHECK(!lh_begin(&m, 0, stamp(0)));
    CHECK(!lh_stop(&m, stamp(0)));
    start(&m);
    CHECK(lh_stop(&m, stamp(0)));
    CHECK(lh_begin(&m, 2, stamp(0)));
    CHECK(lh_stop(&m, stamp(0)));
    CHECK(!lh_begin(&m, 1, stamp(0))); /* 不可回到更舊 session */
    start(&m);
    lh_acquire(&m, LH_SCHED, stamp(10));
    lh_fail(&m, LH_PANIC);
    lh_release(&m, LH_SCHED, stamp(20));
    CHECK(!lh_active(&m) && m.stat[0].count == 0 && m.stat[0].invalid == 1);
    CHECK(m.stat[0].flags & LH_PANIC);
}
static void sources(void)
{
    struct lh_model m;
    struct lh_stamp t = stamp(20);
    start(&m);
    t.valid = 0;
    CHECK(!lh_begin(&m, 2, t)); /* ON 不可重新 begin */
    CHECK(m.flags & LH_INCONSISTENT);
    m = (struct lh_model){0};
    CHECK(!lh_begin(&m, 1, t) && (m.flags & LH_UNSUPPORTED_SOURCE));
    start(&m);
    lh_acquire(&m, LH_SCHED, t);
    CHECK(m.phase == LH_DRAINING && !lh_active(&m));
    CHECK(m.flags & LH_UNSUPPORTED_SOURCE);
    start(&m);
    lh_acquire(&m, LH_SCHED, stamp(10));
    t = stamp(20); t.generation++;
    lh_release(&m, LH_SCHED, t);
    CHECK(m.stat[0].count == 0 && m.stat[0].invalid == 1 && !lh_active(&m));
    CHECK(m.flags & LH_SOURCE_CHANGED); /* A->B->A 來源相同但 generation 不同 */
    start(&m);
    lh_acquire(&m, LH_TIME, stamp(10));
    t = stamp(20); t.source++;
    lh_release(&m, LH_TIME, t);
    CHECK(m.stat[1].invalid == 1 && (m.flags & LH_SOURCE_CHANGED));
    start(&m);
    lh_acquire(&m, LH_SCHED, stamp(10));
    t = stamp(20); t.valid = 0;
    lh_release(&m, LH_SCHED, t);
    CHECK(m.stat[0].invalid == 1 && (m.flags & LH_UNSUPPORTED_SOURCE));
    start(&m);
    pair(&m, LH_SCHED, 20, 19);
    CHECK(m.stat[0].invalid == 1 && (m.flags & LH_BACKWARD));
    start(&m);
    t = stamp(20); t.generation++;
    lh_acquire(&m, LH_SCHED, t);
    CHECK(!lh_active(&m) && (m.flags & LH_SOURCE_CHANGED));
    start(&m);
    pair(&m, LH_SCHED, 10, 20);
    CHECK(lh_stop(&m, t) && (m.flags & LH_SOURCE_CHANGED));
}
static void capacity(void)
{
    struct lh_model m;
    start(&m);
    for (unsigned i = 0; i < LH_RING; i++) pair(&m, LH_SCHED, i, i + 1);
    CHECK(m.stat[0].count == LH_RING && !m.flags && m.stat[0].sum == LH_RING);
    pair(&m, LH_SCHED, 100, 102);
    CHECK(m.stat[0].count == LH_RING + 1 && (m.flags & LH_RAW_OVERWRITTEN));
    CHECK(m.stat[0].ring[0].no == LH_RING && m.stat[0].ring[0].end.count == 102);
    CHECK(m.stat[0].sum == LH_RING + 2 && m.stat[0].max == 2);
    start(&m);
    pair(&m, LH_SCHED, 0, UINT64_MAX);
    CHECK(m.stat[0].sum == UINT64_MAX && !(m.flags & LH_SUM_SAT));
    pair(&m, LH_SCHED, 0, 1);
    CHECK(m.stat[0].sum == UINT64_MAX && (m.flags & LH_SUM_SAT));
    CHECK(m.stat[0].max == UINT64_MAX);
    start(&m);
    m.stat[0].count = UINT32_MAX;
    pair(&m, LH_SCHED, 0, 1);
    CHECK(m.stat[0].count == UINT32_MAX && (m.flags & LH_COUNT_SAT));
    start(&m);
    m.stat[0].invalid = UINT32_MAX;
    pair(&m, LH_SCHED, 2, 1);
    CHECK(m.stat[0].invalid == UINT32_MAX && (m.flags & LH_COUNT_SAT));
}
int main(void)
{
    normal(); lifecycle(); sources(); capacity();
    printf("PASS: lockhold model %u checks\n", checks);
    return 0;
}
