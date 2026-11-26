/*
 * spinlock.c - 自旋鎖,以及每顆 CPU 的狀態陣列(與架構無關)
 */
#include "arch.h"
#include "cpu.h"
#include "panic.h"
#include "smp.h"
#include "spinlock.h"
#include "kprintf.h"
#include "lockhold.h"

struct cpu cpus[MAX_CPUS];
int cpu_count = 1;

static volatile int panic_mode;

_Static_assert(LOCK_DEPTH_MAX == sizeof(((struct cpu *)0)->held) / sizeof(((struct cpu *)0)->held[0]),
               "cpu.held size");

#if LOCK_ORDER_LEARN
/* 量測模式:每一組不同的違反(外層名稱、內層名稱)記一次 */
struct learned_pair { const char *outer, *inner; unsigned ro, ri, count; };
static struct learned_pair learned[64];
static unsigned nlearned, learned_bad;

static void learn(struct spinlock *h, struct spinlock *l)
{
    for (unsigned i = 0; i < nlearned; i++)
        if (learned[i].outer == h->name && learned[i].inner == l->name) {
            learned[i].count++;
            return;
        }
    if (nlearned < 64)
        learned[nlearned++] = (struct learned_pair){ h->name, l->name, h->rank, l->rank, 1 };
}
#endif

void spin_order_report(void)
{
#if LOCK_ORDER_LEARN
    kprintf("lock order: %u distinct nesting(s), %u violating acquisition(s)\n", nlearned, learned_bad);
    for (unsigned i = 0; i < nlearned; i++)
        kprintf("  LOCKORDER %s holding \"%s\"(%u) then \"%s\"(%u) x%u\n",
                learned[i].ro >= learned[i].ri ? "BAD" : "ok ", learned[i].outer, learned[i].ro,
                learned[i].inner, learned[i].ri, learned[i].count);
#endif
}

/* 鎖的層級:要拿 l 時,目前持有的每一把都必須比它低(見 spinlock.h) */
static void order_check(struct cpu *c, struct spinlock *l)
{
    for (int i = 0; i < c->nlocks; i++) {
        struct spinlock *h = c->held[i];
#if LOCK_ORDER_LEARN
        learn(h, l);                         /* 量測模式:每一組巢狀關係都記下(合法的也記) */
#endif
        if (h->rank >= l->rank) {
#if LOCK_ORDER_LEARN
            learned_bad++;
#else
            panic("lock order: cpu %d acquiring \"%s\" (rank %u) while holding \"%s\" (rank %u)",
                  c->id, l->name, l->rank, h->name, h->rank);
#endif
        }
    }
}

/* 記入 / 移出「持有中」。解鎖不一定依相反的順序,所以移出時要找 */
static void held_push(struct cpu *c, struct spinlock *l)
{
    if (c->nlocks >= LOCK_DEPTH_MAX)
        panic("spinlock \"%s\": cpu %d already holds %d spinlocks", l->name, c->id, c->nlocks);
    c->held[c->nlocks++] = l;
}

static void held_remove(struct cpu *c, struct spinlock *l)
{
    for (int i = c->nlocks - 1; i >= 0; i--)
        if (c->held[i] == l) {
            for (int j = i; j < c->nlocks - 1; j++)
                c->held[j] = c->held[j + 1];
            c->nlocks--;
            return;
        }
    panic("spinlock \"%s\": released on cpu %d but missing from its held list", l->name, c->id);
}

/* 本 CPU 是否持有任何設了 probe_id 的鎖(鎖持有量測的安全點用;呼叫時中斷須已關閉)。
   spinlock 只看自己的持有清單,不知道是哪些鎖 */
int spin_held_probed(void)
{
    struct cpu *c = this_cpu();
    for (int i = 0; i < c->nlocks; i++)
        if (c->held[i]->probe_id)
            return 1;
    return 0;
}

void spin_panic_mode(void)
{
    panic_mode = 1;
    lockhold_panic();                /* 之後 hook 不再被呼叫:量測必須標 PANIC、不可當完整(Codex 091 / 092) */
}

void spin_lock(struct spinlock *l)
{
    struct cpu *c = this_cpu();

    if (panic_mode)
        return;
    /* 別顆 CPU 持有時 l->cpu 不會等於自己的編號,所以這個檢查不需要先上鎖 */
    if (l->locked && l->cpu == c->id)
        panic("spinlock \"%s\": recursive acquire on cpu %d", l->name, c->id);
    order_check(c, l);

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
    held_push(c, l);
    if (l->probe_id && __atomic_load_n(&lockhold_gate, __ATOMIC_RELAXED) && !panic_mode)          /* I-349:取得之後才開始計時 */
        lockhold_acquired(l->probe_id);
}

int spin_trylock(struct spinlock *l)
{
    struct cpu *c = this_cpu();

    if (panic_mode)
        return 1;
    if (__atomic_exchange_n(&l->locked, 1u, __ATOMIC_ACQUIRE))
        return 0;
    l->cpu = c->id;
    held_push(c, l);
    if (l->probe_id && __atomic_load_n(&lockhold_gate, __ATOMIC_RELAXED) && !panic_mode)          /* I-349:與 spin_lock 相同 */
        lockhold_acquired(l->probe_id);
    return 1;
}

void spin_unlock(struct spinlock *l)
{
    struct cpu *c = this_cpu();

    if (panic_mode)
        return;
    if (!l->locked || l->cpu != c->id)
        panic("spinlock \"%s\": released on cpu %d but not held by it", l->name, c->id);
    if (l->probe_id && __atomic_load_n(&lockhold_gate, __ATOMIC_RELAXED) && !panic_mode)          /* I-349:釋放之前結束計時(仍持有鎖,資料不會被別人改) */
        lockhold_releasing(l->probe_id);
    l->cpu = -1;
    held_remove(c, l);
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
