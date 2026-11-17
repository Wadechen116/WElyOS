/*
 * runq_test.c - 在一般電腦(Linux)上以多條執行緒測試執行佇列的實作
 *
 * kernel 裡目前只啟動一顆 CPU,無鎖結構最容易出錯的「多方同時操作」在那裡測不到。
 * 執行佇列的介面(include/runq.h)不依賴 kernel 的其他部分,所以可以把同一份
 * 原始碼(kernel/runq_ring.c 或 runq_list.c)直接編譯成一般程式,
 * 讓每條 pthread 扮演一顆 CPU,以真正的並行來測試。
 *
 * 每條執行緒反覆隨機做:取出、放回(隨機優先權、隨機插隊)、暫時留在手上
 * (模擬執行緒睡著)、更改任意節點的優先權。檢查兩件事:
 *   1. 互斥:同一個節點不會同時被兩條執行緒取得(否則 kernel 裡就是
 *      同一條執行緒同時在兩顆 CPU 上執行)
 *   2. 不遺失:結束後把佇列取空,每個節點都恰好出現一次
 *
 * 建置與執行:bash scripts/host-test.sh
 * 直接執行:build/host/runq_ring [每條執行緒的操作次數] [節點數]
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "cpu.h"
#include "runq.h"

#define NCPU   4
#define MAX_NODES 3000
static int NODES = 3000;            /* 預設遠大於環的容量:一定會用到溢出串列 */
#define STASH  64                   /* 每條執行緒最多暫時留在手上的節點數 */

struct item {
    struct rq_node n;               /* 必須是第一個成員:由節點指標轉回 item */
    volatile int held;              /* 1 = 目前被某條執行緒取得 */
    volatile long runs;
};

__thread struct cpu host_cpu;
int cpu_count = NCPU;

static struct item items[MAX_NODES];
static volatile int stop;
static volatile long errors;
static long iterations = 2000000;

static void fail(const char *what, struct item *it)
{
    fprintf(stderr, "FAIL: %s (node %ld)\n", what, (long)(it - items));
    __atomic_fetch_add(&errors, 1, __ATOMIC_RELAXED);
}

/* 小型的亂數產生器(xorshift),每條執行緒自己一份,不互相干擾 */
static uint32_t rnd(uint32_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

static void give_back(struct item *it, uint32_t *seed)
{
    /* 先標記「不在我手上」再放回:一放回去別人就可能立刻取得 */
    __atomic_store_n(&it->held, 0, __ATOMIC_SEQ_CST);
    runq_push(&it->n, (int)(rnd(seed) % RUNQ_LEVELS), rnd(seed) % 4 == 0);
}

static void *worker(void *arg)
{
    int id = (int)(intptr_t)arg;
    uint32_t seed = 0x9E3779B9u * (uint32_t)(id + 1);
    struct item *stash[STASH];
    int nstash = 0;

    host_cpu.id = id;
    for (long i = 0; i < iterations && !stop; i++) {
        uint32_t r = rnd(&seed) % 100;

        if (r < 60) {                           /* 取出一個 */
            struct rq_node *n = runq_pop();
            if (!n)
                continue;
            struct item *it = (struct item *)n;
            int zero = 0;
            if (!__atomic_compare_exchange_n(&it->held, &zero, 1, 0,
                                             __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
                fail("node handed to two CPUs at once", it);
            it->runs++;
            if (nstash < STASH && rnd(&seed) % 3 == 0)
                stash[nstash++] = it;           /* 留在手上一陣子(模擬睡著) */
            else
                give_back(it, &seed);
        } else if (r < 85) {                    /* 把手上的一個放回去(模擬被喚醒) */
            if (nstash > 0)
                give_back(stash[--nstash], &seed);
        } else {                                /* 更改任意節點的優先權 */
            struct item *it = &items[rnd(&seed) % NODES];
            runq_reprio(&it->n, (int)(rnd(&seed) % RUNQ_LEVELS));
        }
    }
    while (nstash > 0)
        give_back(stash[--nstash], &seed);
    return NULL;
}

int main(int argc, char **argv)
{
    pthread_t th[NCPU];

    if (argc > 1)
        iterations = atol(argv[1]);
    if (argc > 2 && atoi(argv[2]) > 0 && atoi(argv[2]) <= MAX_NODES)
        NODES = atoi(argv[2]);              /* 節點少(例如 200)時幾乎只用到環,不碰溢出串列 */
    printf("runq host test: %s\n", runq_name());
    printf("  %d threads, %d nodes, %ld iterations each\n", NCPU, NODES, iterations);

    host_cpu.id = 0;
    runq_init();
    for (int i = 0; i < NODES; i++)
        runq_push(&items[i].n, i % RUNQ_LEVELS, 0);

    for (int i = 0; i < NCPU; i++)
        pthread_create(&th[i], NULL, worker, (void *)(intptr_t)i);
    for (int i = 0; i < NCPU; i++)
        pthread_join(th[i], NULL);

    /* 全部取空:每個節點應該恰好出現一次,而且是由高優先權到低 */
    long drained = 0, total_runs = 0;
    int last = RUNQ_LEVELS;
    struct rq_node *n;
    while ((n = runq_pop()) != NULL) {
        struct item *it = (struct item *)n;
        if (it->held)
            fail("node popped twice while draining", it);
        it->held = 1;
        if (n->prio > last)
            fail("drain order: higher priority came out after a lower one", it);
        last = n->prio;
        drained++;
    }
    for (int i = 0; i < NODES; i++) {
        if (!items[i].held)
            fail("node lost", &items[i]);
        total_runs += items[i].runs;
    }
    /* runq_highest 參考的是各 CPU 留下的提示,提示由各 CPU 自己在下次取出時更新。
       所以先讓每顆「CPU」各取一次(都應該是空的),再確認提示也都清乾淨了 */
    for (int i = 0; i < NCPU; i++) {
        host_cpu.id = i;
        if (runq_pop() != NULL)
            fail("queue not empty after draining", &items[0]);
    }
    if (runq_highest() != -1)
        fail("stale hint left after every CPU found its queue empty", &items[0]);

    printf("  drained %ld of %d nodes, %ld pops during the run, %ld error(s)\n",
           drained, NODES, total_runs, errors);
    printf(errors || drained != NODES ? "RESULT: FAIL\n" : "RESULT: PASS\n");
    return errors || drained != NODES;
}
