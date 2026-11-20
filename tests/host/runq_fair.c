/*
 * runq_fair.c - 執行佇列的「輪流」性質(同優先權的執行緒依序輪到,每輪各一次)
 *
 * 第 50 步(D2,ISSUES N-009)為了追公平性測試偶發的 40 對 21 而寫。
 * 模擬單一 CPU 上的 schedule():目前的執行緒放回佇列尾端,再取出最前面的一個。
 * 兩條一直在算的執行緒應該嚴格輪流(A B A B ...)。
 *
 * 情境 stale:先讓節點 X 留下一張「作廢的號碼牌」(提高優先權時舊的牌不會被移除,
 * 見 runq_ring.c),X 被取走、結束;同一塊記憶體再被新的執行緒重複使用(sched.c 的
 * 執行緒池)。之後 X 和 Y 輪流。
 * 情境 front:作廢的牌留在「插隊位」(被搶佔時放的位置)。
 * 情境 ovf:同一優先權先塞滿環(64 格),讓作廢的牌和新的牌分別落在環與溢出串列。
 * 情境 plain:沒有作廢的牌,作為對照。
 *
 * 判定:60 輪之中,兩者被取到的次數相差不超過 1,而且不會連續被取到兩次。
 * 建置與執行:bash scripts/host-test.sh(或直接 build/host/runq_fair_ring)
 */
#include <stdio.h>
#include "cpu.h"
#include "runq.h"

__thread struct cpu host_cpu;
int cpu_count = 1;

static int run(int stale)
{
    static struct rq_node nodes[4][2], filler[4][70];       /* 每個情境用自己的節點,彼此不留下任何東西 */
    struct rq_node *px = &nodes[stale][0], *py = &nodes[stale][1];
#define x (*px)
#define y (*py)

    if (stale == 2) {
        /* 被搶佔時放進插隊位,接著被提高優先權,從高的那一級被取走 */
        runq_push(&x, 2, 1);
        runq_reprio(&x, 5);
        if (runq_pop() != &x)
            return printf("setup: expected X from level 5\n"), 1;
    } else if (stale == 3) {
        /* 環裡留下 X 的作廢牌,後面再塞 70 個節點(環滿了,後面的落到溢出串列),
           然後 X、Y 才排隊(也在溢出串列)。先來的 70 個必須先被取走:X 不可以插隊 */
        runq_push(&x, 2, 0);
        runq_reprio(&x, 5);
        if (runq_pop() != &x)
            return printf("setup: expected X from level 5\n"), 1;
        for (int i = 0; i < 70; i++)
            runq_push(&filler[stale][i], 2, 0);
        runq_push(&x, 2, 0);
        runq_push(&y, 2, 0);
        for (int i = 0; i < 70; i++) {
            struct rq_node *n = runq_pop();
            if (n == &x || n == &y || !n) {
                printf("FAIL ovf    %s was taken before %d earlier node(s)\n",
                       n == &x ? "X" : n == &y ? "Y" : "nothing", 70 - i);
                while (runq_pop())
                    ;
                return 1;
            }
        }
    } else if (stale) {
        /* 前一條使用這塊記憶體的執行緒:在優先權 2 排隊時被提高到 5(例如防飢餓、
           優先權繼承、即時工作的新週期),從 5 被取走執行,之後結束 */
        runq_push(&x, 2, 0);
        runq_reprio(&x, 5);
        if (runq_pop() != &x)
            return printf("setup: expected X from level 5\n"), 1;
    }
    /* 新的兩條執行緒(X 重複使用同一塊記憶體),同優先權 */
    if (stale != 3) {
        runq_push(&x, 2, 0);
        runq_push(&y, 2, 0);
    }

    int cx = 0, cy = 0, twice = 0;
    struct rq_node *cur = runq_pop(), *last = NULL;
    char seq[64] = {0};
    for (int i = 0; i < 60; i++) {
        if (!cur)
            return printf("queue lost a node at round %d\n", i), 1;
        if (cur == last)
            twice++;
        if (cur == &x) cx++; else cy++;
        if (i < 63) seq[i] = cur == &x ? 'X' : 'Y';
        last = cur;
        runq_push(cur, 2, 0);                /* 時間片用完:排到同級最後 */
        cur = runq_pop();
    }
    while (runq_pop())                       /* 取空:下一個情境從空的佇列開始 */
        ;
    int ok = (cx > cy ? cx - cy : cy - cx) <= 1 && !twice;
    printf("%s %-6s X=%d Y=%d consecutive=%d %s\n", ok ? "PASS" : "FAIL", (const char *[]){"plain", "stale", "front", "ovf"}[stale],
           cx, cy, twice, seq);
    return !ok;
#undef x
#undef y
}

int main(void)
{
    runq_init();
    printf("runq fairness (%s)\n", runq_name());
    int rc = run(0);
    rc |= run(1);
    rc |= run(2);
    rc |= run(3);
    return rc;
}
