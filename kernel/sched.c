/*
 * sched.c - 核心執行緒與排程器:固定優先權、搶佔式、同級輪流(與架構無關)
 *
 * 核心觀念:context switch(切換執行緒)其實就是「換 stack」。
 *   1. 把目前執行緒需要保留的暫存器推到它自己的 stack 上
 *   2. 把 stack pointer 存進它的 struct thread
 *   3. 載入下一條執行緒的 stack pointer
 *   4. 從新的 stack 彈出它上次存的暫存器,然後返回
 * 第 4 步的「返回」會回到下一條執行緒上次呼叫 arch_switch() 的地方,
 * 對它來說就像 arch_switch() 這個函式呼叫剛剛結束一樣。
 * 這四步必須用組合語言寫(arch/<名稱>/switch.S),其餘都是 C。
 *
 * 切換發生的時機有兩種:
 *   主動(cooperative):執行緒自己呼叫 thread_yield / thread_sleep / thread_exit
 *   搶佔(preemptive):計時器中斷發現時間片用完,在中斷返回前呼叫 schedule()
 * 兩種走的是同一個 schedule()。搶佔時,被打斷的暫存器已由中斷入口存在
 * 該執行緒自己的 stack 上(trap frame),所以之後切回來、從中斷返回就能繼續。
 *
 * 上鎖規則(多核心):
 *   排程器的所有資料(執行佇列、休眠串列、所有等待佇列、每條執行緒的狀態與優先權)
 *   由同一把 spinlock「sched_lk」保護,以 sched_lock() / sched_unlock() 取得與釋放。
 *   semaphore、mutex、futex 的內部狀態也由它保護(見 sync.c、proc.c)。
 *
 *   schedule() 一律在持有這把鎖(因此中斷也關閉)的狀態下呼叫,而且鎖會
 *   「跟著 CPU 跨過切換」:舊執行緒上鎖 -> 切換 -> 由新執行緒解鎖。
 *   這是必要的:舊執行緒在切換完成前就已經被放回執行佇列,如果這時鎖已釋放,
 *   別顆 CPU 可能立刻把它取出來執行,兩顆 CPU 就會同時踩在同一條 stack 上。
 *   鎖一直持有到切換完成,別顆 CPU 在那之前拿不到佇列裡的任何東西。
 *
 *   單一把鎖的代價是多顆 CPU 不能同時進行排程相關的操作;以目前的規模
 *   (CPU 數很少、臨界區很短)換來的是正確性容易確認。見 DEVLOG 第 16 步。
 */
#include <stddef.h>
#include "arch.h"
#include "cpu.h"
#include "uapi/errno.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "kstring.h"
#include "panic.h"
#include "pmm.h"
#include "runq.h"
#include "sched.h"
#include "smp.h"
#include "spinlock.h"
#include "timer.h"
#include "vm.h"
#include "membudget.h"

#define STACK_PAGES   4u                     /* 每條執行緒的 stack:16KB */
#define STACK_SIZE    (STACK_PAGES * PAGE_SIZE)
#define TIME_SLICE_MS 10u                    /* 時間片長度(毫秒) */
#define STACK_CACHE_MAX 32u                  /* 最多保留幾條閒置的 stack 供重複使用 */
#define STARVE_MS     100u                   /* 可執行卻等超過這麼久就暫時提升(防飢餓) */
#define STARVE_SCAN_MS 10u                   /* 每隔多久檢查一次 */

enum thread_state {
    T_READY,       /* 在執行佇列中等待輪到 */
    T_RUNNING,     /* 正在使用 CPU */
    T_SLEEPING,    /* 等待時間到 */
    T_BLOCKED,     /* 在某個等待佇列上,等別人喚醒 */
    T_ZOMBIE       /* 已結束,等待別人回收它的 stack */
};

struct thread {
    void *sp;                  /* 被切換出去時的 stack pointer(arch_switch 存取) */
    struct mb_domain *dom;     /* 記帳的 domain(持有一份參考,到 thread_free 才放);kernel 執行緒為 NULL */
    unsigned long kobj;        /* 記在 dom 上的 KOBJ 頁數(kernel stack + 這個結構) */
    int id;
    const char *name;
    enum thread_state state;
    char *stack;               /* 配置區塊的起點(= 保護頁);開機 stack 不是配置來的,為 NULL */
    uintptr_t stack_lo, stack_hi;
    void (*fn)(void *);        /* 進入點與參數 */
    void *arg;
    int prio;                  /* 有效優先權:排程實際使用的值,由下面三項算出(見 update_prio) */
    int base_prio;             /* 自己原本的優先權 */
    int inherit_prio;          /* 經由 mutex 繼承來的優先權;-1 = 沒有 */
    int starve_boost;          /* 1 = 因等太久而暫時提升到最高(防飢餓) */
    uint64_t ready_since;      /* 進入執行佇列時的 tick,用來判斷等了多久 */
    void *blocked_on;          /* 正在等待的 mutex(優先權繼承沿著它傳遞) */
    void *held;                /* 持有的 mutex 串列(由 sync.c 維護) */
    unsigned slice;            /* 剩餘的時間片(tick 數) */
    unsigned long cpu_ticks;   /* 累計被記了多少個 tick 的 CPU 時間 */
    uint64_t wake_tick;        /* T_SLEEPING:到這個 tick 就喚醒 */
    struct thread *next;       /* 串在休眠串列或某個等待佇列中(同時只會在一個) */
    struct wait_queue *wq;     /* T_BLOCKED:睡在哪一個等待佇列上(被終止時要從那裡拿出來) */
    struct rq_node rq;         /* 在執行佇列中的節點(由 runq 的實作使用,見 runq.h) */
    int cpu;                   /* T_RUNNING:正在哪一顆 CPU 上執行 */
    struct thread *all_next;   /* 串在「所有執行緒」串列中,供 thread_join 以編號尋找 */
    struct vm_space *space;    /* 使用的位址空間;NULL = 純 kernel 執行緒 */
    void *data;                /* 附掛資料(所屬的行程) */
    volatile int killed;       /* 被要求終止:返回使用者模式前自行結束 */
    volatile int interrupted;  /* 有訊號等著它的行程處理:正在等的事情先放下,回使用者模式去執行處理函式 */
    struct wait_queue joiners; /* 正在 thread_join 等我結束的執行緒 */
    /* ---- 週期性的即時工作(rt_period 為 0 = 不是);單位都是 tick ---- */
    unsigned rt_period, rt_budget;
    unsigned rt_used;          /* 這個週期已經用掉的 CPU 時間 */
    uint64_t rt_release;       /* 下一個週期開始的時間 = 目前這一次的期限 */
    int rt_waiting;            /* 這一次已經做完,正在睡到下一個週期 */
    int rt_throttled;          /* 這個週期超出預算:降回一般優先權,到下個週期為止 */
    unsigned rt_util;          /* 佔 CPU 的千分比(預算 / 週期) */
    int rt_level;              /* 在即時工作之間的層級(0 = 週期最長的);有效優先權 = PRIO_RT + rt_level */
    unsigned rt_releases, rt_misses, rt_overruns, rt_max_used;
};

/* 開機時 boot.S 設定的 stack,main 執行緒繼續使用它;stack_guard 是它下方的保護頁 */
extern char stack_guard[], stack_bottom[], stack_top[];

static struct thread main_thread;            /* 開機流程本身 */

/* 排程器的鎖:保護本檔所有的共用資料(見檔案開頭的上鎖規則) */
static struct spinlock sched_lk = SPINLOCK_INIT("sched", LK_SCHED);

/* 目前這顆 CPU 正在執行的執行緒。只能在中斷關閉時使用(見 cpu.h) */
static inline struct thread *cur(void) { return this_cpu()->current; }

_Static_assert(RUNQ_LEVELS == PRIO_LEVELS, "runq levels");

/* 由執行佇列的節點找回它所屬的執行緒 */
static inline struct thread *thread_of(struct rq_node *n)
{
    return (struct thread *)((char *)n - offsetof(struct thread, rq));
}

static struct thread *sleepers;              /* 休眠中的執行緒,依喚醒時間由早到晚排序 */
static struct thread *all_threads;           /* 所有存活的執行緒(不含 main 與 idle) */
static unsigned nthreads;                    /* 存活數(不含 idle) */
static unsigned long nswitches;
static int next_id;
static unsigned time_slice = 1;              /* 時間片換算成 tick 數,sched_init 依計時器頻率設定 */
static unsigned starve_ticks = 100;          /* STARVE_MS 換算成 tick 數 */
static unsigned starve_scan_ticks = 10;      /* STARVE_SCAN_MS 換算成 tick 數 */
static unsigned long nstarve_boosts;         /* 累計的防飢餓提升次數 */

/*
 * 即時工作的登記表。數量刻意很少而且固定:每個 tick 都要把它走一遍(檢查期限),
 * 固定的上限讓這件事的時間有界 — 即時系統裡,檢查本身不可以變成不可預測的來源。
 */
#define RT_MAX       8u
#define RT_UTIL_MAX  700u                    /* 即時工作合計最多佔 CPU 的千分比(見 thread_set_periodic) */
static struct thread *rt_tasks[RT_MAX];
static unsigned rt_util_total;

static void schedule(void);
static void thread_free(struct thread *t);
static void update_prio(struct thread *t);
static void rt_begin_period(struct thread *t);
static void rt_check_deadlines(uint64_t now);
static void rt_remove(struct thread *t);
static void rt_assign_levels(void);

/* ---- 排程器的鎖 ---- */

unsigned long sched_lock(void)
{
    return spin_lock_irqsave(&sched_lk);
}

/*
 * 取出這顆 CPU 上等待回收的執行緒(若有)。
 * 執行緒不能釋放自己正踩著的 stack,所以結束時只把自己記在 cpu->zombie,
 * 由「同一顆 CPU 上切換過去的下一條執行緒」在解鎖時代為釋放。
 * 那時切換已經完成,確定沒有人還在使用那條 stack。
 */
static struct thread *take_zombie(void)
{
    struct cpu *c = this_cpu();
    struct thread *z = c->zombie;

    KASSERT(z == NULL || z != c->current);
    c->zombie = NULL;
    return z;
}

void sched_unlock(unsigned long flags)
{
    struct thread *z = take_zombie();

    spin_unlock(&sched_lk);
    /* 釋放記憶體放在解鎖之後:它會用到 heap、頁表、頁面配置器各自的鎖,
       不要讓排程器的鎖包住它們(持有時間越短越好,也避免鎖的順序問題) */
    if (z)
        thread_free(z);
    arch_irq_restore(flags);
}

/* 這顆 CPU 目前執行的優先權;idle 視為 -1(任何執行緒都比它優先) */
static int cpu_running_prio(struct cpu *c)
{
    return c->current == c->idle ? -1 : c->current->prio;
}

/* 要求某顆 CPU 重新排程。對象不是自己時,發處理器間中斷讓它立刻處理
   (否則它要等到自己的下一個計時器 tick 才會發現) */
static void cpu_resched(struct cpu *c)
{
    c->need_resched = 1;
    if (c != this_cpu())
        smp_send(c, IPI_RESCHED);
}

/*
 * 有一條優先權為 prio 的執行緒變成可執行:挑一顆應該讓出來的 CPU,要求它重新排程。
 * 挑「目前執行的優先權最低、而且低於 prio」的那一顆(閒置的最優先);
 * 已經被要求重新排程的 CPU 略過,讓同時被喚醒的多條執行緒分散到不同的 CPU。
 * 從自己開始找:條件相同時留在自己這顆,不必通知別人。
 */
static void kick(int prio)
{
    struct cpu *me = this_cpu(), *best = NULL;
    int best_prio = prio;

    for (int i = 0; i < cpu_count; i++) {
        struct cpu *c = &cpus[(me->id + i) % cpu_count];
        if (!c->current || c->need_resched)
            continue;
        int p = cpu_running_prio(c);
        if (p < best_prio) {
            best_prio = p;
            best = c;
        }
    }
    if (best)
        cpu_resched(best);
}

/*
 * ---- 執行佇列 ----
 * 實際的存放與取出交給 runq(見 runq.h;實作可以替換),這裡只是薄薄的一層:
 * 設定執行緒的狀態、記錄開始等待的時間。
 */

/* 排到自己優先權的最後面 */
static void rq_push(struct thread *t)
{
    t->ready_since = timer_ticks();          /* 從現在開始算等待時間 */
    t->state = T_READY;
    runq_push(&t->rq, t->prio, 0);
}

/* 排到最前面:被更高優先權搶佔的執行緒,時間片還沒用完,
   不該因此排到同級的最後面 */
static void rq_push_front(struct thread *t)
{
    /* 被搶佔的這一刻才開始等待。不重設的話,ready_since 還停在它「上一次」進入佇列的時候 —
       一條從不睡覺的執行緒(一直在算的那種)會帶著建立時的時間,每次被搶佔都像是已經等了
       上百毫秒,於是被防飢餓的機制提升到最高優先權、搶走一整個時間片。
       (第 48 步以重現 40 次的量測找到:被降級的即時工作因此被擋到期限過後) */
    t->ready_since = timer_ticks();
    t->state = T_READY;
    runq_push(&t->rq, t->prio, 1);
}

/* 目前可執行的最高優先權;沒有任何可執行的執行緒回傳 -1 */
static int rq_highest(void)
{
    return runq_highest();
}

/* 取出優先權最高的;同級則大致是排最久的 */
static struct thread *rq_pop(void)
{
    struct rq_node *n = runq_pop();
    return n ? thread_of(n) : NULL;
}

/* 一條執行緒剛變成可執行:排進佇列;若它比某顆 CPU 正在跑的更優先(或有 CPU 閒著),
   要求那顆 CPU 重新排程 */
static void make_ready(struct thread *t)
{
    rq_push(t);
    kick(t->prio);
}

/* ---- 等待佇列 ---- */

void wq_sleep(struct wait_queue *wq)
{
    struct thread *t = cur();

    KASSERT(spin_held(&sched_lk));
    t->state = T_BLOCKED;                    /* 不是 RUNNING,schedule() 就不會把它排回去 */
    t->wq = wq;
    t->next = NULL;
    if (wq->tail)
        wq->tail->next = t;
    else
        wq->head = t;
    wq->tail = t;
    schedule();                              /* 被喚醒、再次輪到時才從這裡返回 */
}

int wq_wake_one(struct wait_queue *wq)
{
    KASSERT(spin_held(&sched_lk));

    /* 找出優先權最高的等待者;用 > 而不是 >=,相同優先權時保留最先排隊的 */
    struct thread *best = NULL, *best_prev = NULL, *prev = NULL;
    for (struct thread *t = wq->head; t; prev = t, t = t->next) {
        if (!best || t->prio > best->prio) {
            best = t;
            best_prev = prev;
        }
    }
    if (best) {
        if (best_prev)
            best_prev->next = best->next;
        else
            wq->head = best->next;
        if (wq->tail == best)
            wq->tail = best_prev;
        best->wq = NULL;
        make_ready(best);
    }
    return best != NULL;
}

int wq_wake_all(struct wait_queue *wq)
{
    int n = 0;
    while (wq_wake_one(wq))
        n++;
    return n;
}

/*
 * 執行緒的 stack:配置 STACK_PAGES + 1 頁,最低的一頁是「保護頁」(guard page),
 * 把它從頁表移除。stack 往低位址成長,用過頭就會碰到保護頁,立刻觸發 page fault,
 * 不會悄悄寫壞相鄰的記憶體。
 *
 *     低位址                                              高位址
 *     | 保護頁(不對映) |        可用的 stack(16KB)        |
 *     ^ stack           ^ stack_lo                          ^ stack_hi
 *
 * 快取:結束的執行緒把 stack 放進 stack_cache,下次建立直接取用。
 * 省下的不只是頁面配置,還有「移除保護頁對映 + 清 TLB」與結束時的「對映回來 + 清 TLB」,
 * 這兩個才是比較貴的操作。閒置的 stack 用它自己的第一個可用位元組串成串列。
 */
static struct spinlock stack_lk = SPINLOCK_INIT("stack-cache", LK_STACKS);
static char *stack_cache;                    /* 閒置 stack 的串列(存的是配置區塊起點) */
static unsigned stack_cached;

static char *stack_alloc(void)
{
    unsigned long flags = spin_lock_irqsave(&stack_lk);
    char *st = stack_cache;
    if (st) {
        stack_cache = *(char **)(st + PAGE_SIZE);
        stack_cached--;
    }
    spin_unlock_irqrestore(&stack_lk, flags);
    if (st)
        return st;

    st = pmm_alloc_pages(STACK_PAGES + 1);
    if (!st)
        return NULL;
    if (vm_unmap((uintptr_t)st) != 0) {      /* 移除保護頁的對映 */
        pmm_free_pages(st, STACK_PAGES + 1);
        return NULL;
    }
    return st;
}

/* 閒置 stack 快取目前佔住的實體頁數(測試核對「記憶體全部回來了」時要扣掉) */
size_t sched_stack_cache_pages(void)
{
    unsigned long flags = spin_lock_irqsave(&stack_lk);
    size_t n = (size_t)stack_cached * (STACK_PAGES + 1);
    spin_unlock_irqrestore(&stack_lk, flags);
    return n;
}

/* 把一條 stack 真正還給 pmm:保護頁要先對映回來,否則下一個拿到這頁的人無法使用 */
static void stack_release(char *st)
{
    if (vm_map((uintptr_t)st, (uintptr_t)st, VM_READ | VM_WRITE) != 0)
        panic("sched: cannot remap guard page %p", (void *)st);
    pmm_free_pages(st, STACK_PAGES + 1);
}

static void stack_free(char *st)
{
    unsigned long flags = spin_lock_irqsave(&stack_lk);
    if (stack_cached < STACK_CACHE_MAX) {
        *(char **)(st + PAGE_SIZE) = stack_cache;
        stack_cache = st;
        stack_cached++;
        st = NULL;
    }
    spin_unlock_irqrestore(&stack_lk, flags);
    if (st)
        stack_release(st);
}

/*
 * struct thread 的池子:結束的執行緒把結構放回這裡重複使用,不還給 heap。
 * 兩個理由:
 *   - 快:建立執行緒不必每次向 heap 配置與釋放
 *   - 安全:無鎖的執行佇列裡可能留有指向已結束執行緒的過期項目(見 runq_ring.c),
 *     它們之後會被讀取。只要這塊記憶體永遠是一個 struct thread,讀取就是安全的。
 *     所以這個池子刻意不提供「還給 heap」的功能
 */
static struct thread *thread_pool;
static size_t pool_bytes;                    /* 池子(含使用中的)累計向 heap 取得的位元組數 */

size_t sched_pool_bytes(void) { return __atomic_load_n(&pool_bytes, __ATOMIC_RELAXED); }

static struct thread *thread_struct_alloc(void)
{
    unsigned long flags = spin_lock_irqsave(&stack_lk);
    struct thread *t = thread_pool;
    if (t)
        thread_pool = t->next;
    spin_unlock_irqrestore(&stack_lk, flags);

    if (!t) {
        t = kzalloc(sizeof(*t));
        if (t)
            __atomic_fetch_add(&pool_bytes, kmalloc_size(t), __ATOMIC_RELAXED);
        return t;
    }
    memset(t, 0, sizeof(*t));
    return t;
}

static void thread_struct_free(struct thread *t)
{
    unsigned long flags = spin_lock_irqsave(&stack_lk);
    t->state = T_ZOMBIE;
    t->next = thread_pool;
    thread_pool = t;
    spin_unlock_irqrestore(&stack_lk, flags);
}

/* 已結束、但 stack 還沒被回收的執行緒數(在 sched_lk 下增加;回收後以原子操作減少) */
static volatile unsigned nzombies;

/* 釋放一條已結束的執行緒(它的 stack 與結構本身) */
/* 退回一條執行緒記在 domain 上的 KOBJ 配額並放掉參考(stack 進快取、結構進 pool 之後就是 kernel 的) */
static void thread_uncharge(struct thread *t)
{
    struct mb_domain *d = t->dom;
    if (!d)
        return;
    t->dom = NULL;
    if (mb_quota_sub(d, MB_KOBJ, t->kobj) != 0 || mb_domain_put(d) != 0)
        panic("sched: thread %s kernel-object accounting mismatch", t->name);
}

static void thread_free(struct thread *t)
{
    thread_uncharge(t);
    stack_free(t->stack);
    thread_struct_free(t);
    __atomic_fetch_sub(&nzombies, 1, __ATOMIC_SEQ_CST);
}

void sched_trim(void)
{
    /* 多核心下,thread_join 返回時對方可能還沒被回收:它在另一顆 CPU 上
       剛宣告結束,stack 要等那顆 CPU 切換到下一條執行緒之後才釋放。
       呼叫者要的是「所有已結束的執行緒都收乾淨」,所以先等到沒有待回收的為止 */
    while (__atomic_load_n(&nzombies, __ATOMIC_SEQ_CST) != 0)
        thread_yield();

    for (;;) {
        unsigned long flags = spin_lock_irqsave(&stack_lk);
        char *st = stack_cache;
        if (st) {
            stack_cache = *(char **)(st + PAGE_SIZE);
            stack_cached--;
        }
        spin_unlock_irqrestore(&stack_lk, flags);
        if (!st)
            break;
        stack_release(st);
    }
}

/*
 * 排程:選出下一條執行緒並切換過去。呼叫前必須持有 sched_lk(中斷因此是關閉的),
 * 而且不可持有其他任何 spinlock:切換出去之後可能很久才回來,
 * 這段期間別人會一直等那把鎖(若對方關著中斷自旋,整個系統就卡住了)。
 * 目前的執行緒若還能跑(RUNNING)就排回佇列;若已改成 SLEEPING / BLOCKED / ZOMBIE 則不排。
 */
/*
 * ---- 排程紀錄 ----
 * 每顆 CPU 保留最近 SCHED_TRACE 次切換:什麼時候、換下誰(狀態、剩餘的時間片、優先權)、換上誰。
 * 用途是讓偶發的排程問題「下次發生時自己說明原因」(docs/ISSUES.md 的 N-009):
 * 測試失敗時呼叫 sched_trace_dump() 印出來。寫入在 sched_lk 之下,每次只是幾個欄位,成本可以忽略。
 */
#define SCHED_TRACE 64u
struct sched_ev {
    uint32_t tick;                       /* 低 32 位元就夠看出先後與間隔 */
    int16_t prev, next;                  /* 執行緒編號;idle 記為 -1(開機的主執行緒是 0) */
    uint8_t prev_state, prev_slice, prev_prio, next_prio;
};
static struct sched_ev strace[MAX_CPUS][SCHED_TRACE];
static unsigned strace_n[MAX_CPUS];

static void trace_switch(struct cpu *c, struct thread *prev, struct thread *next)
{
    struct sched_ev *e = &strace[c->id][strace_n[c->id]++ % SCHED_TRACE];

    e->tick = (uint32_t)timer_ticks();
    e->prev = (int16_t)(prev == c->idle ? -1 : prev->id);
    e->next = (int16_t)(next == c->idle ? -1 : next->id);
    e->prev_state = (uint8_t)prev->state;
    e->prev_slice = (uint8_t)(prev->slice > 255 ? 255 : prev->slice);
    e->prev_prio = (uint8_t)prev->prio;
    e->next_prio = (uint8_t)next->prio;
}

/*
 * 印出第 cpu 顆 CPU 最近 last 次切換,回傳實際印出的筆數。
 * 別顆 CPU 可能正在寫自己的紀錄,所以先在 sched_lk 之下把那一段複製出來(最多 64 筆、不到 1 KB),
 * 解鎖後才印:印字很慢,不能讓排程器的鎖等主控台。
 */
static unsigned trace_dump_one(int cpu, unsigned last)
{
    static const char st[] = "RrSBZ";        /* READY / running / SLEEPING / BLOCKED / ZOMBIE */
    struct sched_ev copy[SCHED_TRACE];

    if (last > SCHED_TRACE)
        last = SCHED_TRACE;
    unsigned long fl = sched_lock();
    unsigned n = strace_n[cpu], k = n < last ? n : last;
    for (unsigned i = 0; i < k; i++)
        copy[i] = strace[cpu][(n - k + i) % SCHED_TRACE];
    sched_unlock(fl);

    kprintf("sched trace, cpu %d, last %u of %u switches (tick: prev[state slice prio] -> next[prio]):\n",
            cpu, k, n);
    for (unsigned i = 0; i < k; i++) {
        const struct sched_ev *e = &copy[i];
        kprintf("  %u: %d[%c %u p%u] -> %d[p%u]\n", e->tick, e->prev,
                e->prev_state < 5 ? st[e->prev_state] : '?', e->prev_slice, e->prev_prio, e->next, e->next_prio);
    }
    return k;
}

void sched_trace_dump(int cpu, unsigned last)
{
    if (cpu >= 0 && cpu < MAX_CPUS)
        (void)trace_dump_one(cpu, last);
}

/*
 * 印出所有參與排程的 CPU 的紀錄。多核心時,出問題的那條執行緒常常剛在別顆 CPU 上跑過,
 * 只印自己這顆會看不到原因。回傳「有紀錄、也印出來了」的 CPU 數,供測試核對。
 */
int sched_trace_dump_all(unsigned last)
{
    int printed = 0;

    for (int cpu = 0; cpu < cpu_count && cpu < MAX_CPUS; cpu++)
        if (trace_dump_one(cpu, last) > 0)
            printed++;
    return printed;
}

static void schedule(void)
{
    struct cpu *c = this_cpu();
    struct thread *prev = c->current;

    KASSERT(spin_held(&sched_lk));
    if (c->nlocks != 1)
        panic("schedule: cpu %d would switch threads while holding %d spinlock(s)",
              c->id, c->nlocks - 1);

    c->need_resched = 0;

    /* 防飢餓的提升只給一次上 CPU 的機會:離開 CPU 時就收回 */
    if (prev->starve_boost) {
        prev->starve_boost = 0;
        prev->prio = prev->inherit_prio > prev->base_prio ? prev->inherit_prio
                                                           : prev->base_prio;
    }

    if (prev->state == T_RUNNING && prev != c->idle) {
        if (prev->slice > 0)
            rq_push_front(prev);             /* 被更優先的搶佔:保留位置與剩餘時間片 */
        else
            rq_push(prev);                   /* 時間片用完或主動讓出:排到同級最後 */
    }

    struct thread *next = rq_pop();          /* 優先權最高的;同級則是排最久的 */
    if (!next)
        next = c->idle;                      /* 所有人都在睡:跑 idle 等中斷 */

    next->state = T_RUNNING;
    next->cpu = c->id;
    if (next->slice == 0)
        next->slice = time_slice;            /* 新的一輪;被搶佔回來的沿用剩餘的 */
    if (next == prev)
        return;                              /* 仍然是自己最優先,不必切換 */

    trace_switch(c, prev, next);
    c->current = next;
    c->kstack_limit = next->stack_lo;        /* 例外入口用它檢查 stack 是否溢位 */
    c->kstack_top = next->stack_hi;
    arch_set_kstack(next->stack_hi);         /* 從使用者模式進入 kernel 時要用的 stack */
    /* 換頁表很貴(TLB 會被清空),所以只在必要時才換:kernel 執行緒不需要
       特定的位址空間,繼續用上一個行程的頁表即可(kernel 部分完全相同) */
    if (next->space && next->space != c->loaded_space) {
        vm_space_switch(next->space);
        c->loaded_space = next->space;
    }
    nswitches++;
    arch_switch(&prev->sp, next->sp);

    /*
     * 執行到這裡時已經是「之後某次被切回來」。注意:切回來時可能已經換到
     * 另一顆 CPU 上了,所以上面的 c 不可以再使用(要用就重新取 this_cpu())。
     * 鎖此刻由「把我們切回來的那條執行緒」所上,交由呼叫者解鎖。
     */
}

/* 新執行緒第一次被切換到時從這裡開始(由 arch_stack_init 安排)。
   此刻持有著上一條執行緒上的鎖(見檔案開頭),由這裡釋放 */
static void thread_entry(void)
{
    struct thread *z = take_zombie();        /* 上一條若剛結束,替它收尾 */
    struct thread *t = cur();

    spin_unlock(&sched_lk);
    if (z)
        thread_free(z);
    arch_irq_enable();
    t->fn(t->arg);
    thread_exit();
}

static void idle_main(void *arg)
{
    (void)arg;
    for (;;)
        arch_idle();                         /* 休眠到下一次中斷;被喚醒者會搶佔它 */
}

static struct thread *thread_alloc(const char *name, void (*fn)(void *), void *arg,
                                   int prio)
{
    struct thread *t = thread_struct_alloc();
    if (!t)
        return NULL;
    t->stack = stack_alloc();
    if (!t->stack) {
        thread_struct_free(t);
        return NULL;
    }
    t->stack_lo = (uintptr_t)t->stack + PAGE_SIZE;       /* 最低一頁是保護頁 */
    t->stack_hi = t->stack_lo + STACK_SIZE;
    t->name = name;
    t->prio = t->base_prio = prio;
    t->inherit_prio = -1;
    t->fn = fn;
    t->arg = arg;
    /* 在新 stack 上預先擺好一份「假的」已存暫存器,
       讓第一次 arch_switch 切過來時會「返回」到 thread_entry */
    t->sp = arch_stack_init((void *)t->stack_hi, thread_entry);
    return t;
}

void sched_init(void)
{
    /* 時間片以毫秒定義,再依實際的 tick 頻率換算,所以改頻率不必改這裡。
       時間片越短反應越快,但切換次數越多、花在切換上的比例越高 */
    time_slice = timer_hz() * TIME_SLICE_MS / 1000;
    if (time_slice == 0)
        time_slice = 1;

    runq_init();
    kprintf("sched: run queue = %s\n", runq_name());

    main_thread.id = next_id++;
    main_thread.name = "main";
    main_thread.state = T_RUNNING;
    main_thread.slice = time_slice;
    main_thread.prio = main_thread.base_prio = PRIO_NORMAL;
    main_thread.inherit_prio = -1;
    starve_ticks = timer_hz() * STARVE_MS / 1000;
    starve_scan_ticks = timer_hz() * STARVE_SCAN_MS / 1000;
    if (starve_scan_ticks == 0)
        starve_scan_ticks = 1;
    main_thread.stack_lo = (uintptr_t)stack_bottom;
    main_thread.stack_hi = (uintptr_t)stack_top;
    nthreads = 1;

    /* 開機 stack 下方由 boot.S 預留了一頁,同樣移除對映當作保護頁 */
    if (vm_unmap((uintptr_t)stack_guard) != 0)
        panic("sched: cannot unmap boot stack guard page");

    /* 開機的這顆 CPU(編號 0)。每顆 CPU 有自己的 idle 執行緒:
       沒有工作時各自休眠,不能共用同一條(一條執行緒不能同時在兩顆 CPU 上執行) */
    struct cpu *c = this_cpu();
    c->kstack_limit = main_thread.stack_lo;
    c->kstack_top = main_thread.stack_hi;
    c->idle = thread_alloc("idle", idle_main, NULL, PRIO_MIN);
    if (!c->idle)
        panic("sched: cannot create idle thread");
    c->idle->id = -1;
    c->online = 1;
    c->current = &main_thread;               /* 從這一刻起排程器算是啟動了 */
}

/*
 * 其他 CPU 加入排程時在自己身上呼叫(中斷關閉中)。
 * 呼叫者目前的執行流程(從啟動一路走到這裡、用的是啟動時配置的 stack)
 * 直接登記成這顆 CPU 的 idle 執行緒,就像開機流程被登記成 main 執行緒一樣。
 */
void sched_init_ap(void)
{
    struct thread *t = thread_struct_alloc();
    if (!t)
        panic("sched: cannot allocate the idle thread of a secondary cpu");

    unsigned long flags = sched_lock();
    struct cpu *c = this_cpu();
    t->id = -1;
    t->name = "idle";
    t->state = T_RUNNING;
    t->prio = t->base_prio = PRIO_MIN;
    t->inherit_prio = -1;
    t->stack_lo = c->kstack_limit;
    t->stack_hi = c->kstack_top;
    t->cpu = c->id;
    c->idle = t;
    c->current = t;
    c->online = 1;
    sched_unlock(flags);
}

void sched_set_cpu_count(int n)
{
    unsigned long flags = sched_lock();
    cpu_count = n;
    sched_unlock(flags);
}

/* 目前這顆 CPU 的編號。只是當下的值:返回時執行緒可能已經被移到別顆 CPU */
int sched_cpu_id(void)
{
    unsigned long flags = arch_irq_save();
    int id = this_cpu()->id;
    arch_irq_restore(flags);
    return id;
}

/*
 * 某個位址空間即將被釋放:如果這顆 CPU 還載入著它,換回 kernel 的頁表。
 * 由 vm.c 以跨 CPU 呼叫在每顆 CPU 上執行(中斷關閉中,只動自己這顆的狀態)。
 * 會發生這種情況是因為「kernel 執行緒沿用上一個行程的頁表」的最佳化:
 * 行程的最後一條執行緒在別顆 CPU 上結束了,這顆 CPU 的 idle 卻還用著它的頁表。
 */
void sched_space_dropped(struct vm_space *vs)
{
    struct cpu *c = this_cpu();

    if (c->loaded_space == vs) {
        vm_space_switch(NULL);
        c->loaded_space = NULL;
    }
}

/* 某個位址空間的對映被大量更改(fork 把頁改成唯讀):這顆 CPU 若正載入著它,
   重新載入並清空它的轉換紀錄。由 vm.c 以跨 CPU 呼叫在每顆 CPU 上執行 */
void sched_space_flush(struct vm_space *vs)
{
    if (this_cpu()->loaded_space == vs)
        vm_space_switch_flush(vs);
}

int thread_create(const char *name, void (*fn)(void *), void *arg)
{
    return thread_create_prio(name, fn, arg, PRIO_NORMAL);
}

struct thread *thread_prepare(const char *name, void (*fn)(void *), void *arg, int prio,
                              struct mb_domain *dom)
{
    if (prio < PRIO_MIN || prio > PRIO_MAX)
        return NULL;
    struct thread *t = thread_alloc(name, fn, arg, prio);
    if (!t)
        return NULL;
    if (dom) {
        /* KOBJ:kernel stack(含保護頁)精確計;結構以 kmalloc 實際配置的大小向上取整成頁 —
           保守的邏輯配額,不拿來推論實體用量(設計 B6) */
        unsigned long k = STACK_PAGES + 1 + (kmalloc_size(t) + PAGE_SIZE - 1) / PAGE_SIZE;
        if (mb_domain_get(dom) != 0) {
            stack_free(t->stack);
            thread_struct_free(t);
            return NULL;
        }
        if (mb_quota_add(dom, MB_KOBJ, k) != 0) {
            if (mb_domain_put(dom) != 0)
                panic("sched: domain reference mismatch");
            stack_free(t->stack);
            thread_struct_free(t);
            return NULL;
        }
        t->dom = dom;
        t->kobj = k;
    }
    return t;
}

struct mb_domain *thread_domain(void)
{
    return cur()->dom;                       /* 只讀自己的欄位;它在執行緒存活期間不變 */
}

void thread_abandon(struct thread *t)
{
    thread_uncharge(t);
    stack_free(t->stack);
    thread_struct_free(t);
}

int thread_start(struct thread *t)
{
    unsigned long flags = sched_lock();
    int id = t->id = next_id++;
    nthreads++;
    t->all_next = all_threads;
    all_threads = t;
    make_ready(t);
    sched_preempt_point();                   /* 新執行緒比自己優先:立刻讓它跑 */
    sched_unlock(flags);
    /* 注意:解鎖之後 t 隨時可能已經跑完並被釋放,所以回傳先記下的 id,不再碰 t */
    return id;
}

int thread_create_prio(const char *name, void (*fn)(void *), void *arg, int prio)
{
    struct thread *t = thread_prepare(name, fn, arg, prio, NULL);
    return t ? thread_start(t) : -1;
}

void thread_join(int tid)
{
    unsigned long flags = sched_lock();

    /* 「尋找」與「睡下去」之間一直持有鎖,對方不可能剛好在中間結束 */
    for (struct thread *t = all_threads; t; t = t->all_next) {
        if (t->id == tid) {
            KASSERT(t != cur());             /* 等自己結束會永遠等下去 */
            wq_sleep(&t->joiners);
            break;                           /* 被喚醒時 t 已結束,不可再存取 */
        }
    }
    sched_unlock(flags);
}

void thread_set_space(struct vm_space *vs)
{
    /* 只動到自己與自己這顆 CPU 的資料,關中斷(不被搶佔、不換 CPU)就夠了 */
    unsigned long flags = arch_irq_save();
    struct cpu *c = this_cpu();
    c->current->space = vs;
    vm_space_switch(vs);
    c->loaded_space = vs;
    arch_irq_restore(flags);
}

struct thread *thread_current(void)
{
    /* 關中斷再讀:否則可能剛取得這顆 CPU 的指標就被搶佔、換到別顆 CPU,
       接著讀到的是「別顆 CPU 正在跑的執行緒」 */
    unsigned long flags = arch_irq_save();
    struct thread *t = cur();
    arch_irq_restore(flags);
    return t;
}

void thread_set_data(void *data) { thread_current()->data = data; }

void *thread_data(void)
{
    struct thread *t = thread_current();
    return t ? t->data : NULL;
}

int thread_killed(void)
{
    struct thread *t = thread_current();
    return t && t->killed;
}

/* 是否該放下正在等的事情:被終止了,或有訊號要處理 */
int thread_interrupted(void)
{
    struct thread *t = thread_current();
    return t && (t->killed || t->interrupted);
}

void thread_clear_interrupt(void)
{
    struct thread *t = thread_current();
    if (t)
        t->interrupted = 0;
}

/* 把一條正在睡的執行緒提早叫醒(不論它在等什麼)。呼叫時持有 sched_lk */
static void wake_early(struct thread *t)
{
    if (t->state == T_BLOCKED && t->wq) {
        struct wait_queue *wq = t->wq;
        struct thread *prev = NULL;

        for (struct thread *c = wq->head; c; prev = c, c = c->next) {
            if (c != t)
                continue;
            if (prev)
                prev->next = t->next;
            else
                wq->head = t->next;
            if (wq->tail == t)
                wq->tail = prev;
            break;
        }
        t->wq = NULL;
        make_ready(t);
    } else if (t->state == T_SLEEPING) {
        for (struct thread **pp = &sleepers; *pp; pp = &(*pp)->next) {
            if (*pp == t) {
                *pp = t->next;
                break;
            }
        }
        make_ready(t);
    }
}

/*
 * 把所有 data 相同的執行緒標記為「應終止」,並把其中正在睡的叫醒。
 *
 * 被標記的執行緒不會被強行中斷,而是自己在返回使用者模式前結束。但它若正睡在
 * kernel 裡(等鍵盤輸入、等管線的資料、等另一個行程結束…),可能永遠不會醒來。
 * 所以這裡直接把它從它睡的地方拿出來、放回執行佇列。
 *
 * 對被叫醒的那段程式來說,這是一次「條件還沒成立就醒來」。所有等待的寫法
 * 本來就要求醒來後重新檢查條件(見 sync.c 開頭);會讓使用者行程等很久的地方
 * 另外會檢查 thread_killed(),看到自己被終止就放棄等待。
 */
void sched_kill_group(void *data)
{
    KASSERT(spin_held(&sched_lk));
    for (struct thread *t = all_threads; t; t = t->all_next) {
        if (t->data != data)
            continue;
        t->killed = 1;
        wake_early(t);
    }
}

/* 讓屬於 data(行程)的每條執行緒放下正在等的事情:標記為「被打斷」並提早叫醒。
   和 sched_kill_group 的差別是執行緒不會結束,只是它所在的系統呼叫提早返回(-E_INTR) */
void sched_interrupt_group(void *data)
{
    KASSERT(spin_held(&sched_lk));
    for (struct thread *t = all_threads; t; t = t->all_next) {
        if (t->data != data)
            continue;
        t->interrupted = 1;
        wake_early(t);
    }
}

/* 統計屬於 data(行程)的執行緒:目前共用掉多少 tick 的 CPU 時間,
   以及有幾條正在執行或等著執行(其餘的在睡)。呼叫時必須持有排程器的鎖 */
void sched_group_usage(void *data, unsigned long *ticks, unsigned *runnable)
{
    KASSERT(spin_held(&sched_lk));
    *ticks = 0;
    *runnable = 0;
    for (struct thread *t = all_threads; t; t = t->all_next) {
        if (t->data != data)
            continue;
        *ticks += t->cpu_ticks;
        *runnable += t->state == T_RUNNING || t->state == T_READY;
    }
}

void sched_preempt_point(void)
{
    struct cpu *c = this_cpu();

    KASSERT(spin_held(&sched_lk));
    if (c->current && c->need_resched && !c->in_irq)
        schedule();
}

/*
 * 重新計算 t 的有效優先權並維持各佇列的一致。呼叫時已持有 sched_lk。
 *   有效優先權 = 正處於防飢餓提升 ? 最高 : max(自己的, 繼承來的)
 */
static void update_prio(struct thread *t)
{
    int prio = t->inherit_prio > t->base_prio ? t->inherit_prio : t->base_prio;
    if (t->starve_boost)
        prio = PRIO_MAX;
    if (t->rt_period && !t->rt_throttled)
        prio = PRIO_RT + t->rt_level;        /* 預算之內的即時工作:高於所有一般的執行緒;彼此之間依週期排序 */
    if (t->prio == prio)
        return;

    if (t->state == T_READY) {
        t->prio = prio;
        runq_reprio(&t->rq, prio);           /* 改到新優先權的佇列;等待時間照舊計算 */
        kick(prio);
    } else {
        t->prio = prio;                      /* RUNNING / SLEEPING / BLOCKED:改欄位即可 */
        /* 正在執行的執行緒被調低後,若有人比它優先,它所在的 CPU 要讓出 */
        if (t->state == T_RUNNING && rq_highest() > prio)
            cpu_resched(&cpus[t->cpu]);
    }
}

void thread_set_priority(int prio)
{
    if (prio < PRIO_MIN || prio > PRIO_MAX)
        return;

    unsigned long flags = sched_lock();
    cur()->base_prio = prio;
    update_prio(cur());
    sched_preempt_point();
    sched_unlock(flags);
}

unsigned long thread_cpu_ticks(void)
{
    struct thread *t = thread_current();
    return t ? t->cpu_ticks : 0;
}

int thread_priority(void)
{
    struct thread *t = thread_current();
    return t ? t->prio : PRIO_NORMAL;
}

int thread_effective_prio(struct thread *t) { return t->prio; }
void *thread_blocked_on(struct thread *t)   { return t->blocked_on; }
void thread_set_blocked_on(void *m)         { cur()->blocked_on = m; }
void **thread_held_list(struct thread *t)   { return &t->held; }

void sched_set_inherit(struct thread *t, int prio)
{
    t->inherit_prio = prio;
    update_prio(t);
}

int wq_max_prio(struct wait_queue *wq)
{
    int best = -1;
    for (struct thread *t = wq->head; t; t = t->next)
        if (t->prio > best)
            best = t->prio;
    return best;
}

/*
 * 防飢餓:找出「可執行卻等了太久」的執行緒,暫時提升到最高優先權。
 * 由計時器 tick 定期呼叫。只需要看每條佇列的頭:同一條佇列裡頭是等最久的。
 * 最高優先權的佇列不必看(它本來就會被執行)。
 * runq_peek 給的只是「候選者」(可能已經被取走、或其實排在別的優先權),
 * 所以每一個都要再確認狀態。
 */
static void starve_scan(uint64_t now)
{
    struct rq_node *cand[RUNQ_PEEK_MAX];

    for (int p = PRIO_MAX - 1; p >= PRIO_MIN; p--) {
        int n = runq_peek(p, cand, RUNQ_PEEK_MAX);
        for (int i = 0; i < n; i++) {
            struct thread *t = thread_of(cand[i]);
            if (t->state != T_READY || t->prio != p || t->starve_boost ||
                now - t->ready_since < starve_ticks)
                continue;
            t->starve_boost = 1;
            nstarve_boosts++;
            update_prio(t);                  /* 移到最高優先權的佇列;必要時要求重新排程 */
        }
    }
}

void thread_yield(void)
{
    unsigned long flags = sched_lock();
    cur()->slice = 0;                        /* 主動放棄剩餘的時間片,排到同級最後 */
    schedule();
    sched_unlock(flags);
}

/* 睡到第 tick 個 tick。呼叫時持有 sched_lk;醒來(時間到,或被提早叫醒)後返回 */
static void sleep_until_locked(struct thread *self, uint64_t tick)
{
    self->wake_tick = tick;
    self->state = T_SLEEPING;

    /* 依喚醒時間插入到正確位置:每個 tick 只需檢查串列的第一個。
       用 <= 讓同一時間到期的維持先來後到的順序 */
    struct thread **pp = &sleepers;
    while (*pp && (*pp)->wake_tick <= self->wake_tick)
        pp = &(*pp)->next;
    self->next = *pp;
    *pp = self;

    schedule();                              /* 醒來後才會從這裡返回 */
}

void thread_sleep(unsigned ms)
{
    /* 毫秒換算成 tick,無條件進位;再加 1 是因為目前這個 tick 已經過了一部分。
       刻意用 32 位元運算:i386 上 64 位元除法需要 libgcc 的 __udivdi3,
       而 freestanding 環境沒有連結它(ms 上限因此約為 4 千萬毫秒,足夠) */
    unsigned n = (ms * timer_hz() + 999) / 1000 + 1;

    unsigned long flags = sched_lock();
    struct thread *self = cur();
    /* 已經被要求終止的執行緒不再睡:它該盡快回到返回使用者模式的檢查點去結束。
       「標記為終止」與這裡的檢查都在同一把鎖之下,所以不會有「剛檢查完就被標記、
       接著照樣睡下去」的空檔 */
    if (self->killed || self->interrupted) {
        sched_unlock(flags);
        return;
    }
    sleep_until_locked(self, timer_ticks_real() + n);   /* 以真實時間為起點:見 timer.h 的 timer_ticks_real */
    sched_unlock(flags);
}

/*
 * ---- 期限排程:週期性的即時工作 ----
 *
 * 一般的執行緒只有「優先權」:誰重要誰先跑,但沒有人承諾「多久之內一定輪得到」。
 * 控制類的工作(每 20 毫秒讀一次感測器、算一次控制量)需要的是另一種保證:
 * 每個週期都準時開始、在期限之前做完;做不到的時候要被發現,而不是悄悄地晚了。
 *
 * 這裡的做法是飛行軟體常見的三件事:
 *   1. 接受前先算(admission control):每個工作宣告「週期」與「每次最多用多少 CPU」。
 *      全部加起來超過 CPU 的 70% 就拒絕 — 系統只承諾做得到的事。
 *      (70% 是保守的界線:固定優先權下,總使用率不超過約 69% 時,不論有幾個工作、
 *       週期怎麼搭配,都保證排得進去。這裡以一顆 CPU 計算,多顆 CPU 時只會更寬裕。)
 *   2. 預算強制(budget enforcement):某個工作用超過它宣告的時間,就在這個週期剩下的時間
 *      降回一般優先權。一個出錯的工作不能拖垮其他準時的工作。
 *   3. 期限監控(deadline monitoring):每個 tick 檢查有沒有工作在新週期開始時還沒做完,
 *      有就記一次錯過。就算那個工作整個卡住、再也不回報,也數得到。
 *
 * 週期以「絕對時間」推進(下一次 = 上一次的預定時間 + 週期,不是「現在」+ 週期),
 * 所以每次晚一點點醒來的誤差不會累積。
 */

/* 新的週期開始:期限往後推一個週期、預算重新算。呼叫時持有 sched_lk */
static void rt_begin_period(struct thread *t)
{
    t->rt_waiting = 0;
    t->rt_release += t->rt_period;
    t->rt_used = 0;
    t->rt_releases++;
    if (t->rt_throttled) {
        t->rt_throttled = 0;
        update_prio(t);
    }
}

/* 每個 tick 由第 0 顆 CPU 呼叫:新的週期到了,工作卻還沒做完的,記一次錯過期限 */
static void rt_check_deadlines(uint64_t now)
{
    for (unsigned i = 0; i < RT_MAX; i++) {
        struct thread *t = rt_tasks[i];
        if (!t || t->rt_waiting || now < t->rt_release)
            continue;
        t->rt_misses++;
        if (t->rt_used > t->rt_max_used)
            t->rt_max_used = t->rt_used;
        /* 下一個週期照常開始(包括新的預算):沒做完的那一次繼續做,佔用的是新週期的時間 */
        t->rt_release += t->rt_period;
        t->rt_used = 0;
        if (t->rt_throttled) {
            t->rt_throttled = 0;
            update_prio(t);
        }
    }
}

/*
 * 重新排定即時工作之間的層級:週期越短,層級越高(rate-monotonic)。呼叫時持有 sched_lk。
 *
 * 為什麼需要:即時工作如果都在同一個層級輪流,一個週期 2 毫秒的工作可能排在一個
 * 正在用它 10 毫秒時間片的長週期工作後面 — 還沒輪到就已經錯過好幾個期限。
 * 「總使用率不超過約 70% 就保證趕得上期限」這個結論,前提正是短週期的工作可以打斷長週期的。
 * 層級 = 有幾種「比我長」的週期;週期相同的在同一層輪流。最多 RT_MAX 個工作,所以層級夠用。
 */
static void rt_assign_levels(void)
{
    for (unsigned i = 0; i < RT_MAX; i++) {
        struct thread *t = rt_tasks[i];
        int level = 0;

        if (!t)
            continue;
        for (unsigned j = 0; j < RT_MAX; j++) {
            struct thread *o = rt_tasks[j];
            int dup = 0;

            if (!o || o->rt_period <= t->rt_period)
                continue;
            for (unsigned k = 0; k < j; k++)             /* 同一種週期只算一次 */
                dup |= rt_tasks[k] && rt_tasks[k]->rt_period == o->rt_period;
            level += !dup;
        }
        if (t->rt_level != level) {
            t->rt_level = level;
            update_prio(t);                  /* 正在排隊的會換到新的層級;必要時要求重新排程 */
        }
    }
}

/* 取消 t 的即時工作身分(本來就不是的話不做任何事)。呼叫時持有 sched_lk */
static void rt_remove(struct thread *t)
{
    if (!t->rt_period)
        return;
    for (unsigned i = 0; i < RT_MAX; i++)
        if (rt_tasks[i] == t)
            rt_tasks[i] = NULL;
    rt_util_total -= t->rt_util;
    t->rt_period = 0;
    t->rt_level = 0;
    t->rt_throttled = t->rt_waiting = 0;
    rt_assign_levels();                      /* 其他工作的相對順序可能因此改變 */
}

int thread_set_periodic(unsigned period_ms, unsigned budget_ms)
{
    unsigned hz = timer_hz();
    int rc = 0;

    /* 上限讓下面的乘法不會溢位,也擋掉沒有意義的宣告 */
    if (period_ms > 60000 || budget_ms > period_ms || (period_ms && !budget_ms))
        return -E_INVAL;
    unsigned period = (period_ms * hz + 999) / 1000, budget = (budget_ms * hz + 999) / 1000;

    unsigned long flags = sched_lock();
    struct thread *self = cur();
    if (period_ms == 0) {
        rt_remove(self);
        update_prio(self);
        sched_preempt_point();               /* 降回一般優先權:有人更優先就讓出 */
        sched_unlock(flags);
        return 0;
    }
    unsigned util = (budget * 1000 + period - 1) / period;       /* 無條件進位:寧可高估 */
    unsigned others = rt_util_total - (self->rt_period ? self->rt_util : 0);
    int slot = -1;
    for (unsigned i = 0; i < RT_MAX; i++) {
        if (rt_tasks[i] == self) {
            slot = (int)i;
            break;
        }
        if (!rt_tasks[i] && slot < 0)
            slot = (int)i;
    }
    if (period < 2 || budget >= period)
        rc = -E_INVAL;                       /* 至少要留一個 tick 給別人,也給「睡到下個週期」 */
    else if (slot < 0)
        rc = -E_NOSPC;
    else if (others + util > RT_UTIL_MAX)
        rc = -E_BUSY;
    if (rc == 0) {
        rt_tasks[slot] = self;
        rt_util_total = others + util;
        self->rt_util = util;
        self->rt_period = period;
        self->rt_budget = budget;
        self->rt_used = 0;
        self->rt_throttled = self->rt_waiting = 0;
        self->rt_releases = self->rt_misses = self->rt_overruns = self->rt_max_used = 0;
        self->rt_release = timer_ticks_real() + period;  /* 第一個週期從現在(真實時間)開始 */
        rt_assign_levels();
        update_prio(self);
    }
    sched_unlock(flags);
    return rc;
}

int thread_wait_period(struct rt_stats *st)
{
    unsigned hz = timer_hz();
    int rc = 0;

    unsigned long flags = sched_lock();
    struct thread *self = cur();
    if (!self->rt_period) {
        sched_unlock(flags);
        return -E_INVAL;
    }
    if (self->rt_used > self->rt_max_used)
        self->rt_max_used = self->rt_used;
    if (self->killed || self->interrupted) {
        rc = -E_INTR;
    } else {
        /* 期限正好在這個 tick、而每個 tick 的檢查還沒輪到:在這裡算,結果才不會取決於誰先跑 */
        uint64_t now = timer_ticks();
        while (self->rt_release <= now) {
            self->rt_misses++;
            self->rt_release += self->rt_period;
            self->rt_used = 0;
        }
        self->rt_waiting = 1;
        sleep_until_locked(self, self->rt_release);
        if (self->rt_waiting) {              /* 不是時間到,而是被提早叫醒(終止、訊號) */
            self->rt_waiting = 0;
            rc = -E_INTR;
        }
    }
    if (st) {
        st->period_ms = self->rt_period * 1000 / hz;
        st->budget_ms = self->rt_budget * 1000 / hz;
        st->releases = self->rt_releases;
        st->misses = self->rt_misses;
        st->overruns = self->rt_overruns;
        st->max_used_ms = self->rt_max_used * 1000 / hz;
        st->throttled = (uint32_t)self->rt_throttled;
        st->total_util = rt_util_total;
    }
    sched_unlock(flags);
    return rc;
}

void thread_exit(void)
{
    (void)sched_lock();                      /* 不會返回,所以不必記住原本的中斷狀態 */
    struct cpu *c = this_cpu();
    struct thread *self = c->current;

    if (self == &main_thread || self == c->idle)
        panic("thread_exit: %s thread must not exit", self->name);
    /* 從「所有執行緒」串列移除:之後的 thread_join 找不到它,會立即返回 */
    for (struct thread **pp = &all_threads; *pp; pp = &(*pp)->all_next) {
        if (*pp == self) {
            *pp = self->all_next;
            break;
        }
    }
    wq_wake_all(&self->joiners);             /* 叫醒所有在等我結束的執行緒 */
    rt_remove(self);                         /* 即時工作:把佔用的 CPU 配額還回去 */

    self->state = T_ZOMBIE;
    c->zombie = self;                        /* 由這顆 CPU 上的下一條執行緒回收 */
    __atomic_fetch_add(&nzombies, 1, __ATOMIC_SEQ_CST);
    nthreads--;
    schedule();
    panic("thread_exit: zombie was scheduled again");
}

/*
 * 每顆 CPU 自己的計時器中斷都會呼叫(中斷關閉中)。
 * 「全系統只需要做一次」的工作(喚醒休眠者、防飢餓檢查)只由第 0 顆 CPU 負責,
 * 它也是唯一推進 tick 計數的 CPU(見 timer.c);時間片則是每顆 CPU 各算各的。
 */
void sched_tick(int charge)
{
    struct cpu *c = this_cpu();

    if (!c->current)
        return;                              /* 排程器尚未啟動 */

    spin_lock(&sched_lk);
    if (c->id == 0) {
        /* 喚醒時間到的休眠者。串列已依喚醒時間排序,所以只看最前面的;
           第一個還沒到期,後面的一定也還沒,可以直接結束 */
        uint64_t now = timer_ticks();
        while (sleepers && now >= sleepers->wake_tick) {
            struct thread *t = sleepers;
            sleepers = t->next;
            if (t->rt_waiting)
                rt_begin_period(t);          /* 即時工作的新週期從這一刻開始 */
            /* 相同優先權:排到隊尾等待輪到,不插隊(見 DEVLOG 第 10 步的公平性問題)。
               比某顆 CPU 正在跑的更優先:make_ready 會要求它立刻重新排程 */
            make_ready(t);
        }

        /* 定期檢查有沒有人等太久(防飢餓)。
           轉成 32 位元再取餘數:i386 上 64 位元的除法 / 餘數需要 libgcc */
        if ((unsigned)now % starve_scan_ticks == 0)
            starve_scan(now);
        rt_check_deadlines(now);
    }

    /* 補記的 tick(中斷晚到的那段時間):時間已經在上面推進過了,但這顆 CPU 當時是被停住的,
       不是被目前這條執行緒用掉的 — 不記 CPU 時間、不扣時間片、不扣即時工作的預算。
       否則每次模擬器停頓,剛好在執行的那條執行緒就會被誤判為用了好幾毫秒(實測過:
       一個幾乎不做事的即時工作因此被判超出預算) */
    if (!charge) {
        spin_unlock(&sched_lk);
        return;
    }

    /* 時間片用完:有「優先權不低於自己」的人在等才換人。
       只有較低優先權的在等時不換,它們本來就輪不到 */
    struct thread *t = c->current;
    t->cpu_ticks++;                          /* 這個 tick 算在正在執行的執行緒頭上 */
    if (t == c->idle)
        c->ticks_idle++;
    else
        c->ticks_busy++;
    /* 即時工作的預算:用超過了,這個週期剩下的時間降回一般優先權。
       它還是可以繼續做(和一般的執行緒一起排隊),只是不再能擋住別人 */
    if (t->rt_period && !t->rt_throttled && ++t->rt_used > t->rt_budget) {
        t->rt_throttled = 1;
        t->rt_overruns++;
        update_prio(t);
        if (rq_highest() >= t->prio)
            c->need_resched = 1;
    }
    /* 閒置中的 CPU:佇列裡(包括別顆 CPU 的)有工作就去拿。平常有工作時會被直接通知,
       這裡是保險,確保不會有「有人在排隊、卻有 CPU 閒著」的情況持續超過一個 tick */
    if (t == c->idle && rq_highest() >= 0)
        c->need_resched = 1;
    if (t != c->idle && t->slice > 0 && --t->slice == 0) {
        /* 靠防飢餓提升上來的執行緒只給一個時間片:用完就讓出,不論別人的優先權高低
           (它此刻是最高優先權,不這樣做的話會反過來霸佔 CPU) */
        int floor = t->starve_boost ? 0 : t->prio;
        if (rq_highest() >= floor)
            c->need_resched = 1;
        else
            t->slice = time_slice;
    }
    spin_unlock(&sched_lk);
}

void sched_irq_enter(void)
{
    this_cpu()->in_irq = 1;
}

void sched_irq_exit(void)
{
    struct cpu *c = this_cpu();

    c->in_irq = 0;
    if (c->current && c->need_resched) {
        unsigned long flags = sched_lock();
        schedule();
        sched_unlock(flags);
    }
}

const char *thread_name(void)
{
    struct thread *t = thread_current();
    return t ? t->name : "boot";
}

int thread_id(void)
{
    struct thread *t = thread_current();
    return t ? t->id : 0;
}

unsigned sched_thread_count(void)      { return nthreads; }
unsigned long sched_switch_count(void) { return nswitches; }
unsigned long sched_starve_boosts(void) { return nstarve_boosts; }

int sched_report_overflow(uintptr_t sp, uintptr_t fault_addr)
{
    uintptr_t lo, hi;

    sched_stack_bounds(&lo, &hi);
    /* 兩種跡象:stack pointer 已經低於可用範圍,或出錯位址落在保護頁內 */
    int sp_out = sp < lo && sp >= lo - STACK_SIZE;
    int in_guard = fault_addr >= lo - PAGE_SIZE && fault_addr < lo;
    if (!sp_out && !in_guard)
        return 0;

    kprintf("\n*** KERNEL STACK OVERFLOW in thread %d (%s) ***\n",
            thread_id(), thread_name());
    kprintf("  stack %p-%p, guard page %p, sp=%p\n", (void *)lo, (void *)hi,
            (void *)(lo - PAGE_SIZE), (void *)sp);
    return 1;
}

void sched_stack_bounds(uintptr_t *lo, uintptr_t *hi)
{
    struct thread *t = thread_current();

    if (t) {
        *lo = t->stack_lo;
        *hi = t->stack_hi;
    } else {
        *lo = (uintptr_t)stack_bottom;
        *hi = (uintptr_t)stack_top;
    }
}
