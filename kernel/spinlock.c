/*
 * spinlock.c - 自旋鎖,以及每顆 CPU 的狀態陣列(與架構無關)
 */
#include "arch.h"
#include "cpu.h"
#include "panic.h"
#include "smp.h"
#include "spinlock.h"

struct cpu cpus[MAX_CPUS];
int cpu_count = 1;

static volatile int panic_mode;

void spin_panic_mode(void)
{
    panic_mode = 1;
}

void spin_lock(struct spinlock *l)
{
    struct cpu *c = this_cpu();

    if (panic_mode)
        return;
    /* 別顆 CPU 持有時 l->cpu 不會等於自己的編號,所以這個檢查不需要先上鎖 */
    if (l->locked && l->cpu == c->id)
        panic("spinlock \"%s\": recursive acquire on cpu %d", l->name, c->id);

    /*
     * 原子交換:把 1 寫進去並取回舊值,整個動作不會被別顆 CPU 插入。
     * 搶不到時,先用一般的讀取等到它看起來空了再重試(test-and-test-and-set):
     * 原子交換每次都要獨佔那條快取線,大家一起反覆交換會互相拖慢;
     * 單純讀取則可以共享,等待的成本低得多。
     */
    while (__atomic_exchange_n(&l->locked, 1u, __ATOMIC_ACQUIRE)) {
        while (l->locked) {
            /*
             * 等待期間順便處理別顆 CPU 的要求(見 smp.h 的跨 CPU 呼叫)。
             * 我們此刻關著中斷,收不到處理器間中斷;如果持有這把鎖的 CPU 正在等
             * 我們回應它的要求,而我們又在等它放鎖,兩邊就永遠互相等待。
             * 在這裡主動檢查,這個循環就不會形成。
             */
            smp_poll();
            arch_cpu_relax();
        }
    }
    l->cpu = c->id;
    c->nlocks++;
}

int spin_trylock(struct spinlock *l)
{
    struct cpu *c = this_cpu();

    if (panic_mode)
        return 1;
    if (__atomic_exchange_n(&l->locked, 1u, __ATOMIC_ACQUIRE))
        return 0;
    l->cpu = c->id;
    c->nlocks++;
    return 1;
}

void spin_unlock(struct spinlock *l)
{
    struct cpu *c = this_cpu();

    if (panic_mode)
        return;
    if (!l->locked || l->cpu != c->id)
        panic("spinlock \"%s\": released on cpu %d but not held by it", l->name, c->id);
    l->cpu = -1;
    c->nlocks--;
    __atomic_store_n(&l->locked, 0u, __ATOMIC_RELEASE);
}

unsigned long spin_lock_irqsave(struct spinlock *l)
{
    unsigned long flags = arch_irq_save();   /* 先關中斷,再上鎖(規則 1) */
    spin_lock(l);
    return flags;
}

void spin_unlock_irqrestore(struct spinlock *l, unsigned long flags)
{
    spin_unlock(l);
    arch_irq_restore(flags);
}

int spin_held(struct spinlock *l)
{
    return panic_mode || (l->locked && l->cpu == this_cpu()->id);
}
