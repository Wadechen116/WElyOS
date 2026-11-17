/*
 * runq_ring.c - 執行佇列的實作之二:每顆 CPU 一組無鎖環狀佇列,加上工作竊取
 *
 * 每顆 CPU 有:
 *   ring[p]   每個優先權一個無鎖環狀佇列(見 ring.h)。只有這顆 CPU 會放入;
 *             自己取出與別顆 CPU 來竊取都不需要鎖
 *   front[p]  每個優先權一格「插隊位」:被搶佔、時間片還沒用完的工作放這裡,
 *             下次輪到這個優先權時最先取出
 *   mask      哪些優先權可能有工作(給別顆 CPU 看的提示,省得它逐一檢查)
 * 全系統共用:
 *   溢出串列  環狀佇列容量固定(64),放不下的進這裡;以一把鎖保護。
 *             它同時是各 CPU 之間自然的平衡點:自己的環空了就來這裡拿
 *
 * 取出的順序(由高優先權到低,同一優先權內):
 *   自己的插隊位 -> 自己的環 -> 溢出串列 -> 別顆 CPU 的插隊位與環(竊取)
 * 「先把同一優先權在所有地方找過一遍,才往下一級」,所以取出的幾乎一定是
 * 全系統優先權最高的(只差在別顆 CPU 同時放入的那一瞬間)。
 *
 * ---- 無鎖結構不能從中間移除,那改優先權怎麼辦? ----
 *
 * 環狀佇列裡的每一格不代表「這個工作在這裡」,而是「這個工作可能可以執行」的一張
 * 「號碼牌」。真正決定誰能執行它的是節點上的旗標 queued:
 *
 *   放入:queued = 1,然後把號碼牌放進環
 *   取出:拿到一張號碼牌後,用 CAS 把 queued 從 1 改成 0。成功才算拿到;
 *         失敗代表這張牌已經作廢(工作早被別人經由另一張牌取走了),丟掉再拿下一張
 *
 * 所以同一個工作可以有好幾張號碼牌,只有第一個兌換的人成功:
 *   提高優先權:不移除舊的牌,直接在新優先權的環再放一張。舊的那張之後被拿到時作廢
 *   降低優先權:什麼都不做。之後從舊的(較高的)環被拿到時,發現優先權不符,
 *               重新放進正確的環
 *
 * 作廢的號碼牌指向的節點可能早就結束了。為了讓「拿到作廢的牌、去看它的 queued」
 * 永遠安全,節點所在的記憶體必須一直是節點(type-stable memory):
 * sched.c 把結束的 struct thread 放回自己的池子重複使用,不還給 heap。
 * 如果那塊記憶體已經被新的執行緒重複使用,而新執行緒正好也在排隊,
 * 經由舊牌把它取走也是正確的:它本來就該被執行,只是提早了一點。
 *
 * ---- 溢出串列與號碼牌的配合 ----
 *
 * 溢出串列用節點自己的 next/prev 串起來,所以一個節點最多只能在串列上出現一次,
 * 而且「已經被取走執行的節點還留在串列上」是不允許的(下次放入會把串列弄壞)。
 * 但節點可能同時有一張環裡的牌,被別顆 CPU 經由那張牌取走。兩邊用這個順序配合:
 *
 *   串入的一方:(持有鎖)串入、where = 在串列上 -> 再讀 queued,若已是 0 就自己拆下來
 *   兌換的一方:CAS 把 queued 改成 0          -> 再讀 where,若在串列上就(持有鎖)拆下來
 *
 * 兩邊都是「先寫自己的、再讀對方的」,而且用最嚴格的記憶體順序(seq_cst),
 * 所以不可能兩邊都沒看到對方:至少有一邊會負責把它拆下來。
 */
#include <stddef.h>
#include <stdint.h>
#include "cpu.h"
#include "ring.h"
#include "runq.h"
#include "spinlock.h"

#define SEQ __ATOMIC_SEQ_CST
#define RLX __ATOMIC_RELAXED

#define W_NONE 0u
#define W_OVF  1u                            /* 串在溢出串列上 */

struct cpu_rq {
    struct ring ring[RUNQ_LEVELS];
    struct rq_node *volatile front[RUNQ_LEVELS];
    volatile unsigned mask;
};
static struct cpu_rq rqs[MAX_CPUS];

static struct spinlock ovf_lk = SPINLOCK_INIT("runq-overflow");
static struct rq_node *ovf_head[RUNQ_LEVELS], *ovf_tail[RUNQ_LEVELS];
static volatile unsigned ovf_count[RUNQ_LEVELS];     /* 各串列的長度(鎖外只當提示讀) */
static volatile unsigned ovf_mask;                   /* bit p = 1:優先權 p 的溢出串列非空 */

void runq_init(void)
{
}

const char *runq_name(void)
{
    return "ring (per-CPU lock-free rings, work stealing)";
}

/* ---- 溢出串列(以下 ovf_* 函式呼叫時都必須持有 ovf_lk) ---- */

static void ovf_unlink(struct rq_node *n)
{
    int p = n->ovf_prio;

    if (n->prev)
        n->prev->next = n->next;
    else
        ovf_head[p] = n->next;
    if (n->next)
        n->next->prev = n->prev;
    else
        ovf_tail[p] = n->prev;
    n->next = n->prev = NULL;
    __atomic_store_n(&ovf_count[p], ovf_count[p] - 1, RLX);
    if (!ovf_head[p])
        __atomic_store_n(&ovf_mask, ovf_mask & ~(1u << p), RLX);
    __atomic_store_n(&n->where, W_NONE, SEQ);
}

static void ovf_link(struct rq_node *n, int p)
{
    n->next = NULL;
    n->prev = ovf_tail[p];
    if (ovf_tail[p])
        ovf_tail[p]->next = n;
    else
        ovf_head[p] = n;
    ovf_tail[p] = n;
    n->ovf_prio = p;
    __atomic_store_n(&ovf_count[p], ovf_count[p] + 1, RLX);
    __atomic_store_n(&ovf_mask, ovf_mask | (1u << p), RLX);
    __atomic_store_n(&n->where, W_OVF, SEQ);
}

/* 把一個 queued = 1 的節點排到溢出串列 p 的尾端(處理了「同時被別人取走」的情況) */
static void ovf_add(struct rq_node *n, int p)
{
    spin_lock(&ovf_lk);
    if (n->where == W_OVF)
        ovf_unlink(n);                       /* 改優先權:先離開原本的串列 */
    ovf_link(n, p);
    if (!__atomic_load_n(&n->queued, SEQ))   /* 串入期間已被別人經由另一張牌取走 */
        ovf_unlink(n);
    spin_unlock(&ovf_lk);
}

static struct rq_node *ovf_take(int p)
{
    struct rq_node *n = NULL;

    if (!(__atomic_load_n(&ovf_mask, RLX) & (1u << p)))
        return NULL;                         /* 看起來是空的:不必上鎖 */
    spin_lock(&ovf_lk);
    n = ovf_head[p];
    if (n)
        ovf_unlink(n);
    spin_unlock(&ovf_lk);
    return n;
}

/* ---- 號碼牌 ---- */

/* 兌換一張號碼牌。成功回傳 1,之後這個節點歸呼叫者所有 */
static int claim(struct rq_node *n)
{
    unsigned one = 1;

    if (!__atomic_compare_exchange_n(&n->queued, &one, 0u, 0, SEQ, SEQ))
        return 0;                            /* 作廢的牌 */
    if (__atomic_load_n(&n->where, SEQ) == W_OVF) {
        spin_lock(&ovf_lk);
        if (n->where == W_OVF)
            ovf_unlink(n);
        spin_unlock(&ovf_lk);
    }
    return 1;
}

/* 在這顆 CPU 的佇列中,替一個 queued = 1 的節點放一張優先權 p 的號碼牌 */
static void put_ticket(struct cpu_rq *q, struct rq_node *n, int p, int front)
{
    __atomic_store_n(&q->mask, __atomic_load_n(&q->mask, RLX) | (1u << p), RLX);
    if (front && !__atomic_load_n(&q->front[p], RLX)) {
        /* 只有擁有者會把插隊位從空改成非空,所以先看再寫是安全的 */
        __atomic_store_n(&q->front[p], n, SEQ);
        return;
    }
    /* 溢出串列有東西時,新來的也排到它後面,維持先進先出;否則放進自己的環 */
    if (__atomic_load_n(&ovf_count[p], RLX) == 0 && ring_push(&q->ring[p], n))
        return;
    ovf_add(n, p);
}

void runq_push(struct rq_node *n, int prio, int front)
{
    __atomic_store_n(&n->prio, prio, RLX);
    __atomic_store_n(&n->queued, 1u, SEQ);   /* 先設旗標再放牌:牌一放出去就可能被兌換 */
    put_ticket(&rqs[this_cpu()->id], n, prio, front);
}

void runq_reprio(struct rq_node *n, int prio)
{
    int old = __atomic_load_n(&n->prio, RLX);

    if (prio == old)
        return;
    __atomic_store_n(&n->prio, prio, RLX);
    if (prio > old)
        put_ticket(&rqs[this_cpu()->id], n, prio, 0);    /* 提高:加一張新的牌 */
    /* 降低:不必動,取出時發現優先權不符會重新排(見 runq_pop) */
}

/* 這顆 CPU 自己的佇列中哪些優先權有東西(直接看,不靠提示) */
static unsigned local_mask(struct cpu_rq *q)
{
    unsigned m = 0;

    for (int p = 0; p < RUNQ_LEVELS; p++)
        if (__atomic_load_n(&q->front[p], RLX) || !ring_empty(&q->ring[p]))
            m |= 1u << p;
    __atomic_store_n(&q->mask, m, RLX);      /* 順便更新給別人看的提示 */
    return m;
}

static unsigned others_mask(int self)
{
    unsigned m = 0;

    for (int i = 0; i < cpu_count; i++)
        if (i != self)
            m |= __atomic_load_n(&rqs[i].mask, RLX);
    return m;
}

/* 從某顆 CPU 的佇列拿一張優先權 p 的號碼牌(自己的或別人的都用這個) */
static struct rq_node *take_from(struct cpu_rq *q, int p)
{
    if (__atomic_load_n(&q->front[p], RLX)) {
        struct rq_node *n = __atomic_exchange_n(&q->front[p], NULL, SEQ);
        if (n)
            return n;
    }
    return ring_take(&q->ring[p]);
}

struct rq_node *runq_pop(void)
{
    int self = this_cpu()->id;
    struct cpu_rq *q = &rqs[self];

    for (;;) {
        unsigned mine = local_mask(q);
        unsigned all = mine | __atomic_load_n(&ovf_mask, RLX) | others_mask(self);
        int requeued = 0;

        if (!all)
            return NULL;
        for (int p = RUNQ_LEVELS - 1; p >= 0 && !requeued; p--) {
            if (!(all & (1u << p)))
                continue;
            /* 依序嘗試各個來源;每個來源拿到作廢的牌就繼續拿,直到它空了 */
            for (int src = -2; src < cpu_count && !requeued; src++) {
                struct rq_node *n;

                if (src == self)
                    continue;
                for (;;) {
                    if (src == -2)
                        n = take_from(q, p);             /* 自己的插隊位與環 */
                    else if (src == -1)
                        n = ovf_take(p);                 /* 溢出串列 */
                    else if (__atomic_load_n(&rqs[src].mask, RLX) & (1u << p))
                        n = take_from(&rqs[src], p);     /* 竊取 */
                    else
                        n = NULL;
                    if (!n)
                        break;
                    if (!claim(n))
                        continue;                        /* 作廢的牌:再拿一張 */
                    int np = __atomic_load_n(&n->prio, RLX);
                    if (np < p) {
                        /* 優先權在排隊期間被調低了:放回正確的位置,整個重新找 */
                        runq_push(n, np, 0);
                        requeued = 1;
                        break;
                    }
                    return n;
                }
            }
        }
        if (!requeued)
            return NULL;                     /* 提示說有,實際上都被拿走了 */
    }
}

int runq_highest(void)
{
    int self = this_cpu()->id;
    unsigned all = local_mask(&rqs[self]) | __atomic_load_n(&ovf_mask, RLX) | others_mask(self);

    for (int p = RUNQ_LEVELS - 1; p >= 0; p--)
        if (all & (1u << p))
            return p;
    return -1;
}

int runq_peek(int prio, struct rq_node **out, int max)
{
    int n = 0;

    for (int i = 0; i < cpu_count && n < max; i++) {
        struct ring *r = &rqs[i].ring[prio];
        struct rq_node *c;

        /* 排在最前面的若是作廢的牌,順手清掉:否則它會一直擋在那裡,
           讓呼叫者看不到後面真正在等的工作(這條環可能很久都輪不到) */
        while ((c = ring_peek(r)) != NULL && !__atomic_load_n(&c->queued, SEQ)) {
            if (!ring_drop_head_if(r, c))
                break;
        }
        if (c)
            out[n++] = c;
        /* 插隊位上的工作同樣可能等很久(更高優先權一直佔著 CPU 時) */
        c = __atomic_load_n(&rqs[i].front[prio], RLX);
        if (c && n < max)
            out[n++] = c;
    }
    if (n < max) {
        spin_lock(&ovf_lk);
        if (ovf_head[prio])
            out[n++] = ovf_head[prio];
        spin_unlock(&ovf_lk);
    }
    return n;
}
