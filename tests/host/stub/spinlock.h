/* 主機測試用的替身:取代 include/spinlock.h(沒有中斷可關,只做互斥) */
#ifndef HOST_STUB_SPINLOCK_H
#define HOST_STUB_SPINLOCK_H
#include <sched.h>
struct spinlock { volatile unsigned locked; const char *name; };
#define SPINLOCK_INIT(nm) { 0, nm }
static inline void spin_lock(struct spinlock *l)
{
    while (__atomic_exchange_n(&l->locked, 1u, __ATOMIC_ACQUIRE))
        sched_yield();
}
static inline void spin_unlock(struct spinlock *l)
{
    __atomic_store_n(&l->locked, 0u, __ATOMIC_RELEASE);
}
#endif
