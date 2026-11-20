/*
 * runq_list.c - 執行佇列的實作之一:全系統共用一組串列,以一把鎖保護
 *
 * 每個優先權一條雙向串列(先進先出),另用一個位元圖記錄哪些串列非空。
 * 取出 = 找位元圖中最高的 1,再取那條串列的頭,成本與工作總數無關。
 * 雙向串列讓「從中間移除」(改優先權時需要)也是固定成本。
 *
 * 優點:單純;全系統只有一組佇列,所以取出的永遠是全系統優先權最高的,順序嚴格。
 * 缺點:所有 CPU 搶同一把鎖,CPU 越多越擠。
 */
#include <stddef.h>
#include "runq.h"
#include "spinlock.h"

static struct spinlock rq_lk = SPINLOCK_INIT("runq", LK_RUNQ);
static struct rq_node *head[RUNQ_LEVELS], *tail[RUNQ_LEVELS];
static unsigned bitmap;                      /* bit p = 1:優先權 p 的串列非空 */

void runq_init(void)
{
}

const char *runq_name(void)
{
    return "list (one global queue, spinlock)";
}

static void link_node(struct rq_node *n, int p, int front)
{
    n->prio = p;
    if (front) {
        n->prev = NULL;
        n->next = head[p];
        if (head[p])
            head[p]->prev = n;
        else
            tail[p] = n;
        head[p] = n;
    } else {
        n->next = NULL;
        n->prev = tail[p];
        if (tail[p])
            tail[p]->next = n;
        else
            head[p] = n;
        tail[p] = n;
    }
    bitmap |= 1u << p;
}

static void unlink_node(struct rq_node *n)
{
    int p = n->prio;

    if (n->prev)
        n->prev->next = n->next;
    else
        head[p] = n->next;
    if (n->next)
        n->next->prev = n->prev;
    else
        tail[p] = n->prev;
    if (!head[p])
        bitmap &= ~(1u << p);
    n->next = n->prev = NULL;
}

static int highest_locked(void)
{
    for (int p = RUNQ_LEVELS - 1; p >= 0; p--)
        if (bitmap & (1u << p))
            return p;
    return -1;
}

void runq_push(struct rq_node *n, int prio, int front)
{
    spin_lock(&rq_lk);
    link_node(n, prio, front);
    n->queued = 1;
    spin_unlock(&rq_lk);
}

struct rq_node *runq_pop(void)
{
    struct rq_node *n = NULL;

    spin_lock(&rq_lk);
    int p = highest_locked();
    if (p >= 0) {
        n = head[p];
        unlink_node(n);
        n->queued = 0;
    }
    spin_unlock(&rq_lk);
    return n;
}

void runq_reprio(struct rq_node *n, int prio)
{
    spin_lock(&rq_lk);
    if (n->queued && n->prio != prio) {
        unlink_node(n);
        link_node(n, prio, 0);
    }
    spin_unlock(&rq_lk);
}

int runq_highest(void)
{
    spin_lock(&rq_lk);
    int p = highest_locked();
    spin_unlock(&rq_lk);
    return p;
}

int runq_peek(int prio, struct rq_node **out, int max)
{
    int n = 0;

    spin_lock(&rq_lk);
    if (max > 0 && head[prio])
        out[n++] = head[prio];
    spin_unlock(&rq_lk);
    return n;
}
