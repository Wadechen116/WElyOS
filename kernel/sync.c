/*
 * sync.c - 號誌與互斥鎖(與架構無關)
 *
 * 兩者的寫法都是同一個模式:
 *
 *     關中斷
 *     while (條件不成立)
 *         wq_sleep(&等待佇列);
 *     取得資源
 *     恢復中斷
 *
 * 兩個重點:
 *
 * 1. 「檢查條件」和「睡下去」必須在同一段關中斷的區間內。
 *    如果中間開了中斷,可能剛檢查完(資源沒有)就被切走,對方釋放資源並
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
    unsigned long flags = arch_irq_save();
    while (s->count == 0)
        wq_sleep(&s->wq);
    s->count--;
    arch_irq_restore(flags);
}

int sem_trywait(struct semaphore *s)
{
    unsigned long flags = arch_irq_save();
    int ok = s->count > 0;
    if (ok)
        s->count--;
    arch_irq_restore(flags);
    return ok;
}

void sem_post(struct semaphore *s)
{
    unsigned long flags = arch_irq_save();
    s->count++;
    wq_wake_one(&s->wq);
    arch_irq_restore(flags);
}

void mutex_init(struct mutex *m)
{
    m->owner = NULL;
    m->wq.head = m->wq.tail = NULL;
}

void mutex_lock(struct mutex *m)
{
    unsigned long flags = arch_irq_save();
    if (m->owner == thread_current())
        panic("mutex: recursive lock by thread %d (%s)", thread_id(), thread_name());
    while (m->owner)
        wq_sleep(&m->wq);
    m->owner = thread_current();
    arch_irq_restore(flags);
}

void mutex_unlock(struct mutex *m)
{
    unsigned long flags = arch_irq_save();
    if (m->owner != thread_current())
        panic("mutex: unlock by non-owner thread %d (%s)", thread_id(), thread_name());
    m->owner = NULL;
    wq_wake_one(&m->wq);
    arch_irq_restore(flags);
}
