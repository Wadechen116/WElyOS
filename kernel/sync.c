/*
 * sync.c - 號誌與互斥鎖(與架構無關)
 *
 * 兩者的寫法都是同一個模式:
 *
 *     flags = sched_lock()          取得排程器的鎖(同時關中斷)
 *     while (條件不成立)
 *         wq_sleep(&等待佇列);
 *     取得資源
 *     sched_unlock(flags)
 *
 * semaphore 與 mutex 的內部狀態(計數、擁有者、等待佇列)和排程器共用同一把鎖。
 * 多核心之下只關中斷是不夠的:別顆 CPU 可能同時在操作同一個 semaphore。
 *
 * 兩個重點:
 *
 * 1. 「檢查條件」和「睡下去」必須在同一段持有鎖的區間內。
 *    如果中間解了鎖,可能剛檢查完(資源沒有)就被切走,對方釋放資源並
 *    發出喚醒(此時佇列是空的,沒人可叫),然後自己才睡下去,
 *    從此再也沒人來叫醒。這種錯誤稱為 lost wakeup。
 *
 * 2. 用 while 而不是 if。被喚醒只代表「條件可能成立了,去看看」,
 *    不保證輪到自己執行時資源還在:從被喚醒到真正開始跑之間,
 *    其他執行緒可能先一步把資源拿走。所以醒來後一定要重新檢查。
 */
#include <stddef.h>
#include "arch.h"
#include "panic.h"
#include "sync.h"

void sem_init(struct semaphore *s, int count)
{
    s->count = count;
    s->wq.head = s->wq.tail = NULL;
}

void sem_wait(struct semaphore *s)
{
    unsigned long flags = sched_lock();
    while (s->count == 0)
        wq_sleep(&s->wq);
    s->count--;
    sched_unlock(flags);
}

int sem_trywait(struct semaphore *s)
{
    unsigned long flags = sched_lock();
    int ok = s->count > 0;
    if (ok)
        s->count--;
    sched_unlock(flags);
    return ok;
}

void sem_post(struct semaphore *s)
{
    unsigned long flags = sched_lock();
    s->count++;
    wq_wake_one(&s->wq);
    sched_preempt_point();                   /* 被喚醒者比自己優先:立刻讓它跑 */
    sched_unlock(flags);
}

/* ------------------------------------------------------------------ */
/* mutex 與優先權繼承                                                   */
/*                                                                    */
/* 每條執行緒有一個「繼承來的優先權」,定義為:                          */
/*     它持有的所有 mutex 上,所有等待者中最高的有效優先權               */
/* 有效優先權 = max(自己的, 繼承來的)。這個定義自然涵蓋兩種複雜情況:    */
/*   - 同時持有多把鎖:解開其中一把後,依「剩下的鎖」重新計算,          */
/*     不會提早失去別把鎖帶來的提升                                    */
/*   - 連鎖等待(H 等 M 持有的鎖,M 又在等 L 持有的鎖):M 的有效優先權   */
/*     因 H 而提高後,要繼續傳給 L,否則 L 還是輪不到                   */
/* ------------------------------------------------------------------ */

#define PI_MAX_DEPTH 8                   /* 沿著等待鏈最多傳遞幾層(也避免死結時無窮迴圈) */

/* 依目前持有的鎖重新計算 t 繼承來的優先權 */
static void recompute_inherit(struct thread *t)
{
    int best = -1;

    for (struct mutex *h = *thread_held_list(t); h; h = h->next_held) {
        if (!h->inherit)
            continue;
        int p = wq_max_prio(&h->wq);
        if (p > best)
            best = p;
    }
    sched_set_inherit(t, best);
}

/* 有人以優先權 prio 開始等 m:把這個優先權沿著「擁有者 -> 擁有者在等的鎖 -> …」傳下去 */
static void propagate_priority(struct mutex *m, int prio)
{
    for (int depth = 0; m && m->inherit && depth < PI_MAX_DEPTH; depth++) {
        struct thread *owner = m->owner;
        if (!owner || thread_effective_prio(owner) >= prio)
            break;                           /* 擁有者已經夠優先,不必再往下 */
        sched_set_inherit(owner, prio);
        m = thread_blocked_on(owner);        /* 擁有者自己也在等別的鎖:繼續傳 */
    }
}

void mutex_init(struct mutex *m)
{
    m->owner = NULL;
    m->inherit = 1;
    m->wq.head = m->wq.tail = NULL;
    m->next_held = NULL;
}

void mutex_lock(struct mutex *m)
{
    struct thread *self = thread_current();
    unsigned long flags = sched_lock();

    if (m->owner == self)
        panic("mutex: recursive lock by thread %d (%s)", thread_id(), thread_name());
    while (m->owner) {
        /*
         * 優先權繼承:持有鎖的人優先權比我低時,把它暫時提高到和我一樣。
         * 否則會發生「優先權反轉」:中等優先權的執行緒不斷搶走 CPU,
         * 持有鎖的低優先權執行緒沒機會執行到解鎖,結果高優先權的我
         * 被一個和這把鎖毫無關係的中等優先權執行緒無限期擋住。
         */
        propagate_priority(m, thread_priority());
        thread_set_blocked_on(m);
        wq_sleep(&m->wq);
        thread_set_blocked_on(NULL);
    }
    m->owner = self;

    /* 記入「我持有的鎖」;還有別人在等這把鎖的話,我要繼承他們的優先權 */
    void **held = thread_held_list(self);
    m->next_held = *held;
    *held = m;
    recompute_inherit(self);
    sched_unlock(flags);
}

void mutex_unlock(struct mutex *m)
{
    struct thread *self = thread_current();
    unsigned long flags = sched_lock();

    if (m->owner != self)
        panic("mutex: unlock by non-owner thread %d (%s)", thread_id(), thread_name());

    /* 從「我持有的鎖」移除 */
    for (void **pp = thread_held_list(self); *pp; pp = (void **)&((struct mutex *)*pp)->next_held) {
        if (*pp == m) {
            *pp = m->next_held;
            break;
        }
    }
    m->next_held = NULL;
    m->owner = NULL;
    recompute_inherit(self);                 /* 只交還這把鎖帶來的提升,其餘的保留 */
    wq_wake_one(&m->wq);
    sched_preempt_point();                   /* 等鎖的人比自己優先:立刻讓它跑 */
    sched_unlock(flags);
}
