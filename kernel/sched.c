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
#include "failinj.h"
#include "smp.h"
#include "spinlock.h"
#include "timer.h"
#include "vm.h"
#include "membudget.h"
#include "mmu.h"

#define STACK_PAGES   4u                     /* 每條執行緒的 stack:16KB */
#define STACK_SIZE    (STACK_PAGES * PAGE_SIZE)
#define TIME_SLICE_MS 10u                    /* 時間片長度(毫秒) */
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
    unsigned long tag;         /* 擁有者自訂的標記(行程模組記這條執行緒用哪一格使用者 stack);0 = 沒有 */
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
    /* ---- CPU 親和性(affinity,I-343);全部以 0 為預設,所以從 pool 取出時 memset 即是「不限制」 ---- */
    unsigned allow;            /* 允許的 CPU 遮罩(bit i = 第 i 顆);0 = 全部(不固定,走 runq) */
    int pin_cpu;               /* 排在第 (pin_cpu - 1) 顆 CPU 的固定佇列上;0 = 不在固定佇列 */
    int pin_prio;              /* 串在固定佇列的哪一個優先權(取下時用;prio 可能已經改了) */
    struct thread *pin_next, *pin_prev;
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
/*
 * 每顆 CPU 的即時工作負載(I-344,千分比)。接受一個即時工作的三條規則(使用者決定,
 * decision:welyos-i344-rt-admission,選 A):
 *   1. 單一工作 <= RT_UTIL_MAX:一條執行緒同一時間只能在一顆 CPU 上跑
 *   2. 全系統合計 <= RT_UTIL_MAX × 線上 CPU 數(多核心的容量)。以前是 RT_UTIL_MAX 一個總量,
 *      那樣任何一顆的負載都不可能超過它,第 3 條永遠不會先觸發(等於沒有作用)
 *   3. 有限制 CPU 的(單一或多顆)把同一份使用率記在遮罩內的每一顆(保守),每顆 <= RT_UTIL_MAX;
 *      不限制的(allow == 0)只計入全系統
 * 保證範圍(明訂):第 2 條保證總量;第 3 條保證「固定在某一顆的」即時工作合計不超過。不固定的即時工作
 * 可以被排到某一顆而和那顆的固定工作合計超過 RT_UTIL_MAX — 這不是逐顆的完整可排程性證明(Codex 018)。
 * 前提:沒有 CPU 熱插拔(online 遮罩開機後不變),記帳不需要隨 CPU 上下線重算。
 */
static unsigned rt_util_cpu[MAX_CPUS];

/*
 * ---- CPU 親和性(affinity,I-343)----
 *
 * 有限制的執行緒(allow != 0)不進 runq:runq 的環可以被任何 CPU 竊取,而它的介面刻意不認得
 * struct thread(見 runq.h),在那裡面加「誰能拿」會把排程策略漏進存放層。所以它們排在
 * 每顆 CPU 一組的「固定佇列」(pinqs)上,由 sched_lk 保護(所有呼叫 runq 的路徑本來就持有它)。
 *
 * 不變式(KASSERT 檢查):
 *   P1  t 在 pinqs[i] 上 ⇒ t 允許第 i 顆(改親和性時,不再允許的會立刻被搬走)
 *   P2  第 c 顆只會從 runq、自己的固定佇列、別顆固定佇列「各優先權的頭且允許 c 的」取出
 *       — 每次最多看 PRIO_LEVELS × MAX_CPUS 個頭,時間有界
 *   P3  從 runq 取到「排隊期間才被限制」而不允許 c 的,不執行,改排到允許的 CPU(最多 PICK_DIVERT_MAX 次)
 * 已知取捨:同優先權的 runq 與固定佇列之間以輪替決定先後(pin_tie),不是嚴格的先進先出。
 */
#define PICK_DIVERT_MAX 8
struct pinq {
    struct thread *head[PRIO_LEVELS], *tail[PRIO_LEVELS];
    struct thread *scan[PRIO_LEVELS];        /* 飢餓掃描下次從哪一個開始(NULL = 從頭);見 starve_scan */
    unsigned mask;                           /* bit p = 1:優先權 p 非空 */
};
static struct pinq pinqs[MAX_CPUS];
#define PIN_SCAN_MAX RUNQ_PEEK_MAX           /* 每層佇列最多看幾個(與 runq 的 peek 相同) */
static unsigned char pin_tie[MAX_CPUS];

/* 負對照:定義後,取出時忽略親和性(固定測試必須因此失敗);正式建置絕不定義 */
#ifdef SCHED_AFFINITY_NEGCTL
#define AFF_IGNORE 1
#else
#define AFF_IGNORE 0
#endif

static inline int allowed_on(const struct thread *t, int cpu)
{
    return t->allow == 0 || ((t->allow >> cpu) & 1u);
}

static unsigned online_mask_locked(void)
{
    unsigned m = 0;
    for (int i = 0; i < cpu_count && i < MAX_CPUS; i++)
        if (cpus[i].online)
            m |= 1u << i;
    return m;
}

static void schedule(void);
static void thread_free(struct thread *t);
static void update_prio(struct thread *t);
static void rt_begin_period(struct thread *t);
static void rt_check_deadlines(uint64_t now);
static void rt_remove(struct thread *t);
static void rt_assign_levels(void);
static void rt_charge_cpus(unsigned allow, unsigned util, int add);
static int rt_cpus_fit(unsigned allow, unsigned util);
static void rt_check_accounting(void);

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

/* 取出優先權最高的;同級則大致是排最久的 */
static struct thread *rq_pop(void)
{
    struct rq_node *n = runq_pop();
    return n ? thread_of(n) : NULL;
}

/* 一條執行緒剛變成可執行:排進佇列;若它比某顆 CPU 正在跑的更優先(或有 CPU 閒著),
   要求那顆 CPU 重新排程 */
/* ---- 固定佇列(呼叫時持有 sched_lk) ---- */

static void pin_link(struct thread *t, int cpu, int front)
{
    struct pinq *q = &pinqs[cpu];
    int p = t->prio;

    KASSERT(t->pin_cpu == 0 && allowed_on(t, cpu));          /* P1 */
    t->pin_cpu = cpu + 1;
    t->pin_prio = p;
    if (front) {
        t->pin_prev = NULL;
        t->pin_next = q->head[p];
        if (q->head[p])
            q->head[p]->pin_prev = t;
        else
            q->tail[p] = t;
        q->head[p] = t;
    } else {
        t->pin_next = NULL;
        t->pin_prev = q->tail[p];
        if (q->tail[p])
            q->tail[p]->pin_next = t;
        else
            q->head[p] = t;
        q->tail[p] = t;
    }
    q->mask |= 1u << p;
}

static void pin_unlink(struct thread *t)
{
    KASSERT(t->pin_cpu > 0);
    struct pinq *q = &pinqs[t->pin_cpu - 1];
    int p = t->pin_prio;

    if (t->pin_prev)
        t->pin_prev->pin_next = t->pin_next;
    else
        q->head[p] = t->pin_next;
    if (t->pin_next)
        t->pin_next->pin_prev = t->pin_prev;
    else
        q->tail[p] = t->pin_prev;
    if (q->scan[p] == t)
        q->scan[p] = t->pin_next;            /* 掃描位置指著它:往後移一個(NULL = 下次從頭) */
    if (!q->head[p])
        q->mask &= ~(1u << p);
    t->pin_next = t->pin_prev = NULL;
    t->pin_cpu = 0;
}

/*
 * 不變式(Codex 004 查證):front=1 的唯一呼叫點是 schedule() 把本 CPU 正在執行、被搶佔且時間片未用完的
 * prev 放回本 CPU 的固定佇列;其他入隊一律尾插。一個節點要被前插必須先在執行,而執行前已離開佇列,
 * 所以每條佇列每層最多只有一個「反覆被前插的新節點」。兩層防飢餓保障:
 *   1. 就算只看前 PIN_SCAN_MAX 個,這個不變式下也足以看到被它擋住的舊節點;
 *   2. 掃描位置(scan)不依賴這個不變式,日後新增前插來源時上界仍成立(I-347)。
 * 新增 front=1 的呼叫點時,必須重新檢查第 1 點。
 */
static void pin_push(struct thread *t, int cpu, int front)
{
    t->ready_since = timer_ticks();
    t->state = T_READY;
    pin_link(t, cpu, front);
}

/* 第 cpu 顆可以從固定佇列取出的最高優先權者:自己的最高層,以及別顆各層的頭(允許 cpu 的)。
   相同優先權時自己的優先。成本有界:最多 MAX_CPUS × PRIO_LEVELS 個頭(P2) */
static struct thread *pin_best(int cpu)
{
    struct thread *best = NULL;

    for (int i = 0; i < cpu_count && i < MAX_CPUS; i++) {
        int c = (cpu + i) % cpu_count;       /* 從自己開始:同優先權時自己的先 */
        unsigned m = pinqs[c].mask;
        for (int p = PRIO_LEVELS - 1; p >= 0 && m; p--) {
            if (!(m & (1u << p)))
                continue;
            if (best && p <= best->prio)
                break;                       /* 這顆剩下的都不會更好 */
            /* 不只看頭(Codex 第 69 步審查):別顆的佇列可能是 [A(只允許那顆), B(也允許這顆)],
               只看頭時 B 要等 A,這顆閒著也拿不到。和 runq 的 peek 一樣最多看 PIN_SCAN_MAX 個,
               成本仍有上限;自己的佇列頭一定允許自己(P1),不會多走 */
            struct thread *h = pinqs[c].head[p];
            for (int k = 0; h && k < PIN_SCAN_MAX; k++, h = h->pin_next)
                if (AFF_IGNORE || allowed_on(h, cpu))
                    break;
            if (h && (AFF_IGNORE || allowed_on(h, cpu))) {
                best = h;
                break;
            }
        }
    }
    return best;
}

/* 第 cpu 顆「可以執行」的最高優先權(runq + 固定佇列);-1 = 沒有 */
static int highest_for(int cpu)
{
    int r = runq_highest();
    struct thread *pt = pin_best(cpu);
    return pt && pt->prio > r ? pt->prio : r;
}

/* 有限制的執行緒要排到哪一顆:允許的 CPU 中,正在執行的優先權最低且低於它的(閒置最優先);
   都不低於它就排在第一顆允許的(從自己開始找)。mask 已在設定時驗證過非空、都在線上 */
static int choose_home(struct thread *t)
{
    int me = this_cpu()->id, best = -1, fallback = -1, best_prio = t->prio;

    for (int i = 0; i < cpu_count; i++) {
        int id = (me + i) % cpu_count;
        struct cpu *c = &cpus[id];
        if (!allowed_on(t, id) || !c->online)
            continue;
        if (fallback < 0)
            fallback = id;
        if (!c->current || c->need_resched)
            continue;
        int p = cpu_running_prio(c);
        if (p < best_prio) {
            best_prio = p;
            best = id;
        }
    }
    KASSERT(fallback >= 0);
    return best >= 0 ? best : fallback;
}

/* 一條執行緒剛變成可執行:排進佇列;若它比某顆 CPU 正在跑的更優先(或有 CPU 閒著),
   要求那顆 CPU 重新排程。有親和性限制的排到允許的 CPU 的固定佇列,只通知那一顆 */
static void make_ready(struct thread *t)
{
    if (!t->allow) {
        rq_push(t);
        kick(t->prio);
        return;
    }
    int h = choose_home(t);
    struct cpu *c = &cpus[h];
    pin_push(t, h, 0);
    if (c->current && !c->need_resched && cpu_running_prio(c) < t->prio)
        cpu_resched(c);
}

/* schedule() 用:這顆 CPU 下一個要執行的。不會傳回不允許這顆的執行緒(P2、P3) */
static struct thread *pick_next(struct cpu *c)
{
    for (int tries = 0; tries < PICK_DIVERT_MAX; tries++) {
        struct thread *pt = pin_best(c->id);
        int pp = pt ? pt->prio : -1, rp = runq_highest();

        if (pt && (pp > rp || (pp == rp && (pin_tie[c->id] ^= 1)))) {
            pin_unlink(pt);
            return pt;
        }
        struct thread *n = rq_pop();
        if (!n) {
            if (pt)
                pin_unlink(pt);
            return pt;
        }
        if (AFF_IGNORE || allowed_on(n, c->id))
            return n;
        make_ready(n);                       /* P3:排隊期間被限制了,改排到允許的 CPU */
    }
    c->need_resched = 1;                     /* 次數用完(極少):先跑 idle,下次中斷返回再找 */
    return NULL;
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
 * 執行緒的 stack(I-339):來自「stack 區塊」(chunk)。每個區塊是一塊對齊 2MB 的實體記憶體
 * (= 直接對映的一個大分頁),切成 KSTACK_SLOTS 格,每格 STACK_PAGES + 1 頁,最低的一頁是「保護頁」:
 *
 *     低位址                                              高位址
 *     | 保護頁(不對映) |        可用的 stack(16KB)        |
 *     ^ stack           ^ stack_lo                          ^ stack_hi
 *
 * 區塊建立時把每格的保護頁一次移除(只拆這一個大分頁、最多多一張頁表),之後永遠不再對映回來。
 * 區塊用完就再建一個(需求驅動,沒有固定的執行緒上限);區塊建好就留著重複使用,不歸還:
 *   - 歸還就得把保護頁對映回去並合併回大分頁;合併(表格 -> 大分頁)在 AArch64 必須
 *     break-before-make,那段期間整個 2MB 沒有對映,別的 CPU 的 stack / heap 可能正在裡面,不安全
 *   - 留著的代價有上限:區塊數 <= ceil(同時存在的 stack 最高數 / KSTACK_SLOTS) + 並行建立的 CPU 數,
 *     與執行緒建立 / 結束的次數無關(churn 不會再拆任何大分頁)
 * 執行期間(區塊夠用時)配置 / 歸還只是位元圖的一個位元:不改頁表、不配置頁表、不需要 TLB shootdown。
 * 區塊尾端剩下的頁(512 - KSTACK_SLOTS * 5 = 2 頁)的最後一頁放區塊的描述(串列、位元圖),不用 heap。
 * 歸還時核對位址與位元(不是區塊裡的格、沒對齊、重複歸還都會 panic:故障偵測,不讓錯誤擴散)。
 */
#define KSTACK_CHUNK_PAGES ((size_t)1 << (MMU_LARGE_LEVEL * MMU_BITS))   /* 512:一個 2MB 大分頁 */
#define KSTACK_CHUNK_BYTES ((uintptr_t)KSTACK_CHUNK_PAGES * PAGE_SIZE)
#define KSTACK_SLOT_PAGES  (STACK_PAGES + 1u)
#define KSTACK_SLOT_BYTES  ((uintptr_t)KSTACK_SLOT_PAGES * PAGE_SIZE)
#define KSTACK_SLOTS       ((unsigned)(KSTACK_CHUNK_PAGES / KSTACK_SLOT_PAGES))   /* 102 */
#define KSTACK_MAP_WORDS   ((KSTACK_SLOTS + 31u) / 32u)
_Static_assert(KSTACK_CHUNK_PAGES - (size_t)KSTACK_SLOTS * KSTACK_SLOT_PAGES >= 1,
               "a chunk needs one spare page for its descriptor");

struct kstack_chunk {                        /* 放在區塊的最後一頁(不在任何一格裡) */
    struct kstack_chunk *next;
    char *base;                              /* 區塊起點(第 0 格的保護頁),對齊 KSTACK_CHUNK_BYTES */
    unsigned used;
    uint32_t map[KSTACK_MAP_WORDS];          /* bit = 1:使用中 */
};
_Static_assert(sizeof(struct kstack_chunk) <= PAGE_SIZE, "descriptor fits in the spare page");

static struct spinlock stack_lk = SPINLOCK_INIT("stack-cache", LK_STACKS);
static struct kstack_chunk *kstack_chunks;   /* 受 stack_lk 保護;只加不減 */
static unsigned kstack_nchunks, kstack_used, kstack_hwm;   /* 受 stack_lk 保護 */

/* 向 pmm 要一塊對齊 KSTACK_CHUNK_BYTES 的 KSTACK_CHUNK_PAGES 頁。pmm 沒有對齊配置,
   所以多要 KSTACK_CHUNK_PAGES - 1 頁,再把頭尾多出來的還回去(每頁參考計數各自獨立,可以分段歸還) */
/* 一整個 2MB 對齊的區塊:pmm 直接找對齊的位置。以前先要 2 × 512 − 1 頁再切出對齊的一段,
   需要約 4MB 連續空閒頁,記憶體零碎時明明有對齊的 2MB 空著也拿不到 */
static char *chunk_pages_alloc(void)
{
    return pmm_alloc_pages_aligned(KSTACK_CHUNK_PAGES, KSTACK_CHUNK_PAGES);
}

/* 建一個新區塊並掛上串列。不可持有 stack_lk:pmm 配置、vm_unmap(拆大分頁 + TLB shootdown 的跨 CPU 呼叫)。
   失敗回傳 -1、什麼都不變 */
static int kstack_chunk_grow(void)
{
    char *base = chunk_pages_alloc();
    if (!base)
        return -1;
    for (unsigned i = 0; i < KSTACK_SLOTS; i++) {
        if (vm_unmap((uintptr_t)base + i * KSTACK_SLOT_BYTES) == 0)
            continue;
        /* 整個區塊在同一個 2MB 大分頁裡:只有第一次 unmap 需要配置頁表(拆開),之後不會失敗 */
        if (i != 0)
            panic("sched: guard unmap %u failed after the chunk was split", i);
        pmm_free_pages(base, KSTACK_CHUNK_PAGES);
        return -1;
    }
    struct kstack_chunk *c = (struct kstack_chunk *)(base + KSTACK_CHUNK_BYTES - PAGE_SIZE);
    memset(c, 0, sizeof(*c));
    c->base = base;
    unsigned long flags = spin_lock_irqsave(&stack_lk);
    c->next = kstack_chunks;
    kstack_chunks = c;
    kstack_nchunks++;
    spin_unlock_irqrestore(&stack_lk, flags);
    return 0;
}

static char *stack_take_locked(void)
{
    for (struct kstack_chunk *c = kstack_chunks; c; c = c->next) {
        if (c->used == KSTACK_SLOTS)
            continue;
        for (unsigned w = 0; w < KSTACK_MAP_WORDS; w++) {
            uint32_t freebits = ~c->map[w];
            if (w == KSTACK_MAP_WORDS - 1 && KSTACK_SLOTS % 32u)
                freebits &= (1u << (KSTACK_SLOTS % 32u)) - 1u;      /* 最後一個字只有前面幾個位元是真的格 */
            if (!freebits)
                continue;
            unsigned b = 0;                      /* 最低的 1:手寫掃描。__builtin_ctz 在沒有 Zbb 的 riscv64 會呼叫 libgcc 的 __ctzdi2(kernel 沒有連結 libgcc) */
            while (!(freebits & (1u << b)))
                b++;
            c->map[w] |= 1u << b;
            c->used++;
            if (++kstack_used > kstack_hwm)
                kstack_hwm = kstack_used;
            return c->base + (w * 32u + b) * KSTACK_SLOT_BYTES;
        }
        panic("sched: kernel stack chunk %p used count %u disagrees with its bitmap", (void *)c->base, c->used);
    }
    return NULL;
}

static char *stack_alloc(void)
{
    for (;;) {
        unsigned long flags = spin_lock_irqsave(&stack_lk);
        char *st = stack_take_locked();
        spin_unlock_irqrestore(&stack_lk, flags);
        if (st)
            return st;
        /* 全部的區塊都滿了:在鎖外建一個新的再重試(別的 CPU 同時也可能建了一個,重試時一併可用) */
        if (kstack_chunk_grow() != 0)
            return NULL;                     /* 呼叫者照「記憶體不足」處理 */
    }
}

/* 從串列拿掉一個區塊。持有 stack_lk;回傳它 */
static struct kstack_chunk *kstack_unlink_locked(struct kstack_chunk *c)
{
    for (struct kstack_chunk **pp = &kstack_chunks; *pp; pp = &(*pp)->next)
        if (*pp == c) {
            *pp = c->next;
            kstack_nchunks--;
            return c;
        }
    panic("sched: kernel stack chunk %p not on the list", (void *)c->base);
}

/* 把一個已拿下的全空區塊還給 pmm(不可持有 stack_lk:vm_map 可能做跨 CPU 的 TLB 清除)。
   保護頁先重新對映(恆等對映),頁才能交給別人用。拆開的頁表留著 — 合併回大分頁在 aarch64 上
   要 break-before-make、不安全(見 DEVLOG 第 68 步);它的上限是實體記憶體 / 2MB,與執行緒數無關 */
static void kstack_chunk_release(struct kstack_chunk *c)
{
    char *base = c->base;                    /* c 在區塊的最後一頁:先記下,釋放之後就不能再碰 */
    for (unsigned i = 0; i < KSTACK_SLOTS; i++) {
        uintptr_t g = (uintptr_t)base + i * KSTACK_SLOT_BYTES;
        if (vm_map(g, g, VM_READ | VM_WRITE) != 0)
            panic("sched: cannot remap guard page %p", (void *)g);
    }
    pmm_free_pages(base, KSTACK_CHUNK_PAGES);
}

static void stack_free(char *st)
{
    char *base = (char *)((uintptr_t)st & ~(KSTACK_CHUNK_BYTES - 1));
    uintptr_t off = (uintptr_t)st - (uintptr_t)base;
    unsigned long flags = spin_lock_irqsave(&stack_lk);
    struct kstack_chunk *c = kstack_chunks;
    while (c && c->base != base)
        c = c->next;
    if (!c || off % KSTACK_SLOT_BYTES != 0 || off / KSTACK_SLOT_BYTES >= KSTACK_SLOTS)
        panic("sched: freeing %p which is not a kernel stack slot", (void *)st);
    unsigned i = (unsigned)(off / KSTACK_SLOT_BYTES);
    if (!(c->map[i / 32u] & (1u << (i % 32u))) || c->used == 0)
        panic("sched: kernel stack %p freed twice", (void *)st);
    c->map[i / 32u] &= ~(1u << (i % 32u));
    c->used--;
    kstack_used--;
    /* 區塊全空、而且已經另有一塊空區塊:這塊還給 pmm(最多保留一塊備用)。
       只增不減的話,一次 1000 條執行緒的高峰會永久佔住約 20MB(stress 測試抓到) */
    struct kstack_chunk *victim = NULL;
    if (c->used == 0) {
        unsigned empty = 0;
        for (struct kstack_chunk *e = kstack_chunks; e; e = e->next)
            if (e->used == 0)
                empty++;
        if (empty > 1)
            victim = kstack_unlink_locked(c);
    }
    spin_unlock_irqrestore(&stack_lk, flags);
    if (victim)
        kstack_chunk_release(victim);
}

/* 歸還所有全空的區塊(sched_trim 用:測試核對「記憶體全部回來了」之前) */
static void kstack_release_empty(void)
{
    for (;;) {
        unsigned long flags = spin_lock_irqsave(&stack_lk);
        struct kstack_chunk *e = kstack_chunks;
        while (e && e->used != 0)
            e = e->next;
        struct kstack_chunk *victim = e ? kstack_unlink_locked(e) : NULL;
        spin_unlock_irqrestore(&stack_lk, flags);
        if (!victim)
            return;
        kstack_chunk_release(victim);
    }
}

void sched_kstack_stats(struct kstack_stats *out)
{
    unsigned long flags = spin_lock_irqsave(&stack_lk);
    out->used = kstack_used;
    out->hwm = kstack_hwm;
    out->chunks = kstack_nchunks;
    out->slots_per_chunk = KSTACK_SLOTS;
    spin_unlock_irqrestore(&stack_lk, flags);
}

/* 區塊目前佔住的實體頁數(全空的區塊最多留一塊備用;sched_trim 會全部歸還) */
size_t sched_kstack_pages(void)
{
    unsigned long flags = spin_lock_irqsave(&stack_lk);
    size_t n = (size_t)kstack_nchunks * KSTACK_CHUNK_PAGES;
    spin_unlock_irqrestore(&stack_lk, flags);
    return n;
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
    kstack_release_empty();                  /* 全空的 kernel stack 區塊(含備用的那塊)還給 pmm */
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

/* 診斷(I-350 階段看門狗):不取鎖的快照,只在 panic 之前用。串列可能正被別顆 CPU 改,所以每個走訪都有上限 */
static const char *const state_name[] = { "ready", "run", "sleep", "block", "zombie" };

static void dump_thread(const char *tag, const struct thread *t)
{
    unsigned st = (unsigned)t->state;
    int pinned = t->pin_cpu ? (int)t->pin_cpu - 1 : -1;   /* 不在固定佇列上 = -1(Codex 015) */
    kprintf("  %s id %d %s: %s prio %d (base %d, boost %d) allow %x pinq %d slice %u ready_since %llu wake %llu\n",
            tag, t->id, t->name ? t->name : "?", st < 5 ? state_name[st] : "?", t->prio, t->base_prio,
            t->starve_boost, t->allow, pinned, t->slice, (unsigned long long)t->ready_since,
            (unsigned long long)t->wake_tick);
}

void sched_dump_state(void)
{
    /* 不可以呼叫 timer_ticks / timer_get_stats:前者在序號為奇數時無限重試、後者要取鎖 — 卡住的
       正可能是時間的寫入端,傾印就會跟著卡住、永遠 panic 不了(Codex 015)。用有限次的讀取 */
    struct timer_stats ts;
    uint64_t now = 0;
    int ok = timer_diag_peek(&now, &ts);
    kprintf("sched state: ticks %s%llu, shared clock %d", (ok & 1) ? "" : "(unreadable) ",
            (unsigned long long)now, timer_shared_enabled());
    if (ok & 2)
        kprintf("; timer catch-ups 2:%u 3:%u 4-5:%u 6:%u 7-9:%u 10+:%u, degraded dropped %u", ts.burst[0],
                ts.burst[1], ts.burst[2], ts.burst[3], ts.burst[4], ts.burst[5], ts.degraded_dropped);
    else
        kprintf("; timer stats lock busy");
    kprintf("\n");
    for (int i = 0; i < cpu_count && i < MAX_CPUS; i++) {
        struct cpu *c = &cpus[i];
        kprintf(" cpu %d: online %d need_resched %d heartbeat %lu pinq mask %x\n", i, c->online,
                c->need_resched, c->heartbeat, pinqs[i].mask);
        if (c->current)
            dump_thread("current", c->current);
    }
    int k = 0;
    for (const struct thread *t = sleepers; t && k < 32; t = t->next, k++)
        dump_thread("sleeper", t);
    k = 0;
    for (const struct thread *t = all_threads; t && k < 128; t = t->all_next, k++)
        dump_thread("thread", t);
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
        if (prev->allow && allowed_on(prev, c->id))
            pin_push(prev, c->id, prev->slice > 0);  /* 有限制、仍允許這顆:排回這顆的固定佇列 */
        else if (prev->allow)
            make_ready(prev);                /* 親和性已不含這顆:排到允許的 CPU 並通知它(I-343 搬移)。
                                                鎖持有到切換完成,那顆 CPU 在那之前拿不到它 */
        else if (prev->slice > 0)
            rq_push_front(prev);             /* 被更優先的搶佔:保留位置與剩餘時間片 */
        else
            rq_push(prev);                   /* 時間片用完或主動讓出:排到同級最後 */
    }

    struct thread *next = pick_next(c);      /* 優先權最高、允許這顆的;同級則大致是排最久的 */
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

void thread_set_tag(unsigned long tag) { cur()->tag = tag; }   /* 只動自己的欄位 */
unsigned long thread_tag(void) { return cur()->tag; }

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

    if (t->state == T_READY && t->pin_cpu) {
        int h = t->pin_cpu - 1;              /* 固定佇列:同一顆 CPU 換到新優先權的尾端 */
        struct cpu *c = &cpus[h];
        pin_unlink(t);
        t->prio = prio;
        pin_link(t, h, 0);
        if (c->current && !c->need_resched && cpu_running_prio(c) < prio)
            cpu_resched(c);
    } else if (t->state == T_READY) {
        t->prio = prio;
        runq_reprio(&t->rq, prio);           /* 改到新優先權的佇列;等待時間照舊計算 */
        kick(prio);
    } else {
        t->prio = prio;                      /* RUNNING / SLEEPING / BLOCKED:改欄位即可 */
        /* 正在執行的執行緒被調低後,若有人(能在那顆 CPU 上跑的)比它優先,它所在的 CPU 要讓出 */
        if (t->state == T_RUNNING && highest_for(t->cpu) > prio)
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
            /* now < ready_since:這條執行緒是在這一輪補記中才就緒的 — 推進時間的 CPU 先把系統時間寫到最新,
               再逐個 tick 補做全系統的工作,就緒時記下的 ready_since(= 最新的 tick)可能比正在補做的 now 新。
               以前直接相減,無號數下溢成極大值,剛醒的執行緒立刻被當成「等太久」提升到最高優先權
               (I-337 之後才出現:優先權延遲測試偶發量到同優先權延遲 1,排程紀錄顯示剛醒的執行緒是 p7) */
            if (t->state != T_READY || t->prio != p || t->starve_boost ||
                now < t->ready_since || now - t->ready_since < starve_ticks)
                continue;
            t->starve_boost = 1;
            nstarve_boosts++;
            update_prio(t);                  /* 移到最高優先權的佇列;必要時要求重新排程 */
        }
    }
    /* 固定佇列(I-343 / I-347):每層從上次停下的位置繼續看 PIN_SCAN_MAX 個,看到尾端下次從頭開始。
       只看頭(第 69 步)或每次都從頭看前 24 個(第 70 步第一版)都不夠:被搶佔的執行緒插回最前面、
       ready_since 是新的,一再插入就能讓後面等很久的永遠落在看得到的範圍之外(Codex host 模型
       tests/host/sched/starve_scan_probe.c)。插到前面的不會影響掃描位置往後推進,所以長度 L 的
       佇列,每個節點至少每 ceil(L / PIN_SCAN_MAX) + 1 次掃描被看一次(每 starve_scan_ticks 掃一次):
       等了 starve_ticks 之後,最多再過 (ceil(L / PIN_SCAN_MAX) + 1) × starve_scan_ticks 個 tick 就會被提升
       (L 不超過執行緒總數)。成本與以前相同,每層每次掃描最多 24 個。
       update_prio 會把 t 移出這層(pin_unlink 同時更新掃描位置),所以先記下下一個再處理 */
    for (int i = 0; i < cpu_count && i < MAX_CPUS; i++) {
        for (int p = PRIO_MAX - 1; p >= PRIO_MIN; p--) {
            struct pinq *q = &pinqs[i];
            struct thread *t = q->scan[p] ? q->scan[p] : q->head[p], *next = NULL;
            for (int k = 0; t && k < PIN_SCAN_MAX; k++, t = next) {
                next = t->pin_next;
                if (t->starve_boost || now < t->ready_since || now - t->ready_since < starve_ticks)
                    continue;
                KASSERT(t->state == T_READY && t->prio == p);
                t->starve_boost = 1;
                nstarve_boosts++;
                update_prio(t);
            }
            q->scan[p] = t;                  /* 停在這裡:下次從它開始;NULL = 看到尾端了,下次從頭 */
        }
    }
}

/*
 * 設定執行緒的 CPU 親和性(I-343)。tid = 0 表示自己(main 也只能以 0 設定自己)。
 * mask 必須非空、而且每一位都是在線上的 CPU,否則 -E_INVAL;找不到 tid 回傳 -E_SRCH。
 * mask 等於全部在線上的 CPU 時存成「不限制」(走 runq,可被竊取)。
 * 目前所在的 CPU 不在新的 mask 裡:要求那顆 CPU 重新排程;它在 schedule() 裡被排到允許的 CPU。
 * 設定自己時,返回前就已經在允許的 CPU 上(搶佔點在這裡;在中斷處理中呼叫則延到中斷返回)。
 */
/* tid 0 = 自己;找不到(或已結束)回傳 NULL。呼叫時持有 sched_lk */
static struct thread *find_thread_locked(int tid)
{
    if (tid == 0)
        return cur();
    for (struct thread *x = all_threads; x; x = x->all_next)
        if (x->id == tid)
            return x;                        /* all_threads 只含存活的:結束的(zombie)找不到 */
    return NULL;
}

/*
 * 設定 t 的親和性。呼叫時持有 sched_lk,mask 已確認非 0、只含線上的 CPU。
 * t 是即時工作時(I-344):先扣掉舊遮罩上的記帳,新遮罩放不下就還原、回 -E_BUSY,
 * 遮罩、佇列、記帳都不變;放得下才提交。
 */
static int affinity_set_locked(struct thread *t, unsigned mask, unsigned online)
{
    unsigned allow = mask == online ? 0 : mask;
    if (t->rt_period) {
        rt_charge_cpus(t->allow, t->rt_util, 0);
        if (!rt_cpus_fit(allow, t->rt_util)) {
            rt_charge_cpus(t->allow, t->rt_util, 1);
            return -E_BUSY;
        }
        rt_charge_cpus(allow, t->rt_util, 1);
    }
    {
        t->allow = allow;
        if (t->state == T_READY && t->pin_cpu) {
            if (!t->allow || !allowed_on(t, t->pin_cpu - 1)) {
                pin_unlink(t);
                make_ready(t);               /* P1:不再允許原本那顆,或改成不限制 */
            }
        } else if (t->state == T_RUNNING && !allowed_on(t, t->cpu)) {
            cpu_resched(&cpus[t->cpu]);      /* 別顆:送 IPI;自己:下面的搶佔點 */
        }
        /* T_READY 且在 runq 中:無法從無鎖的環中間移除,由 pick_next 取到時改排(P3)。
           T_SLEEPING / T_BLOCKED:醒來時 make_ready 依新的 mask 排 */
    }
    rt_check_accounting();
    return 0;
}

int thread_set_affinity(int tid, unsigned mask)
{
    unsigned long flags = sched_lock();
    unsigned online = online_mask_locked();
    struct thread *t = NULL;
    int rc = 0;

    if (mask == 0 || (mask & ~online))
        rc = -E_INVAL;
    else if (!(t = find_thread_locked(tid)))
        rc = -E_SRCH;
    if (rc == 0)
        rc = affinity_set_locked(t, mask, online);
    if (rc == 0 && t == cur())
        sched_preempt_point();
    sched_unlock(flags);
    return rc;
}

/*
 * 使用者層(SYS_setaffinity / SYS_getaffinity,I-344):查 tid、檢查權限、檢查遮罩、檢查即時工作記帳、
 * 提交,全部在同一段 sched_lk 裡 — 不會「先對一條執行緒查權限,實際改到的卻是另一條」(Codex 013)。
 * 權限:同一個行程的執行緒(記帳 domain 相同;fork 會建新的 domain,exec 沿用,所以 domain 對應行程),
 * 或 admin(CAP_ADMIN)。kernel 執行緒(domain 為 NULL)只有 admin 可以設。
 */
static int may_touch(const struct thread *t, const struct mb_domain *dom, int admin)
{
    return admin || (t->dom && t->dom == dom);
}

int thread_set_affinity_checked(int tid, unsigned mask, const struct mb_domain *dom, int admin)
{
    unsigned long flags = sched_lock();
    unsigned online = online_mask_locked();
    struct thread *t = NULL;
    int rc = 0;

    if (mask == 0 || (mask & ~online))
        rc = -E_INVAL;
    else if (!(t = find_thread_locked(tid)))
        rc = -E_SRCH;
    else if (t != cur() && !may_touch(t, dom, admin))
        rc = -E_PERM;
    if (rc == 0)
        rc = affinity_set_locked(t, mask, online);
    if (rc == 0 && t == cur())
        sched_preempt_point();
    sched_unlock(flags);
    return rc;
}

int thread_get_affinity_checked(int tid, const struct mb_domain *dom, int admin, unsigned *out)
{
    unsigned long flags = sched_lock();
    struct thread *t = find_thread_locked(tid);
    int rc = 0;
    if (!t)
        rc = -E_SRCH;
    else if (t != cur() && !may_touch(t, dom, admin))
        rc = -E_PERM;
    else
        *out = t->allow ? t->allow : online_mask_locked();   /* 不限制 = 所有線上的 CPU */
    sched_unlock(flags);
    return rc;
}

/* 自我測試(I-344):全系統目前的即時工作負載(千分比) */
unsigned sched_rt_util_total(void)
{
    unsigned long flags = sched_lock();
    unsigned u = rt_util_total;
    sched_unlock(flags);
    return u;
}

/* 自我測試(I-344):某一顆 CPU 目前的即時工作負載(千分比) */
unsigned sched_rt_util_cpu(int cpu)
{
    unsigned long flags = sched_lock();
    unsigned u = (cpu >= 0 && cpu < MAX_CPUS) ? rt_util_cpu[cpu] : 0;
    sched_unlock(flags);
    return u;
}

unsigned sched_online_mask(void)
{
    unsigned long flags = sched_lock();
    unsigned m = online_mask_locked();
    sched_unlock(flags);
    return m;
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
    /* 測試(I-334):睡下去的這一刻,行程若有可交付的待處理訊號而打斷標記是 0,
       這次睡眠就不會被它叫醒 — 記一筆。只在自我測試註冊了檢查函式時才做 */
    if (sched_sleep_check && self->data && sched_sleep_check(self->data))
        failinj_note_sleep_pending();
    sleep_until_locked(self, timer_wait_base() + n);    /* 以真實時間為起點:見 timer.h 的 timer_wait_base */
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

/* 線上的 CPU 數。不用 __builtin_popcount:它會呼叫 libgcc 的 __popcountsi2,這裡沒有連結 libgcc */
static unsigned online_count_locked(void)
{
    unsigned n = 0;
    for (unsigned m = online_mask_locked(); m; m &= m - 1)
        n++;
    return n;
}

/* 每顆 CPU 的記帳加減(只有有限制的才記;見 rt_util_cpu)。呼叫時持有 sched_lk */
static void rt_charge_cpus(unsigned allow, unsigned util, int add)
{
    for (int c = 0; allow && c < MAX_CPUS; c++)
        if (allow & (1u << c))
            rt_util_cpu[c] = add ? rt_util_cpu[c] + util : rt_util_cpu[c] - util;
}

/* 遮罩內每一顆再加 util 都不超過上限?(呼叫端先扣掉自己舊的那一份) */
static int rt_cpus_fit(unsigned allow, unsigned util)
{
    for (int c = 0; allow && c < MAX_CPUS; c++)
        if ((allow & (1u << c)) && rt_util_cpu[c] + util > RT_UTIL_MAX)
            return 0;
    return 1;
}

/* 不變式:記帳等於從 rt_tasks 重算的值。三個改動點(設定、取消、改親和性)之後都檢查:加減不一致
   (例如某條路徑忘了扣)會在發生的當下 panic,而不是很久以後「莫名其妙被拒絕」 */
static void rt_check_accounting(void)
{
    unsigned total = 0, cpu[MAX_CPUS] = { 0 };
    for (unsigned i = 0; i < RT_MAX; i++) {
        struct thread *t = rt_tasks[i];
        if (!t)
            continue;
        total += t->rt_util;
        for (int c = 0; t->allow && c < MAX_CPUS; c++)
            if (t->allow & (1u << c))
                cpu[c] += t->rt_util;
    }
    KASSERT(total == rt_util_total);
    for (int c = 0; c < MAX_CPUS; c++)
        KASSERT(cpu[c] == rt_util_cpu[c]);
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
    rt_charge_cpus(t->allow, t->rt_util, 0);
    t->rt_period = 0;
    t->rt_level = 0;
    t->rt_throttled = t->rt_waiting = 0;
    rt_assign_levels();                      /* 其他工作的相對順序可能因此改變 */
    rt_check_accounting();
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
    /* 每顆 CPU(I-344):先扣掉自己舊的那一份(重新宣告時),檢查新的放不放得下;放不下就還原 */
    if (self->rt_period)
        rt_charge_cpus(self->allow, self->rt_util, 0);
    if (period < 2 || budget >= period)
        rc = -E_INVAL;                       /* 至少要留一個 tick 給別人,也給「睡到下個週期」 */
    else if (slot < 0)
        rc = -E_NOSPC;
    else if (util > RT_UTIL_MAX || others + util > RT_UTIL_MAX * online_count_locked() ||
             !rt_cpus_fit(self->allow, util))
        rc = -E_BUSY;                        /* 見 rt_util_cpu 上方的三條規則(I-344) */
    if (rc != 0 && self->rt_period)
        rt_charge_cpus(self->allow, self->rt_util, 1);   /* 拒絕:舊的設定完整保留 */
    if (rc == 0) {
        rt_tasks[slot] = self;
        rt_util_total = others + util;
        rt_charge_cpus(self->allow, util, 1);
        self->rt_util = util;
        self->rt_period = period;
        self->rt_budget = budget;
        self->rt_used = 0;
        self->rt_throttled = self->rt_waiting = 0;
        self->rt_releases = self->rt_misses = self->rt_overruns = self->rt_max_used = 0;
        /* 第一個週期從現在開始。用 timer_wait_base() 而不是 timer_ticks_real():降級時(I-346)第一個
           晚到的中斷照常 +1,與 sleep、timer_deadline_ms 用同一個起點,降級時只會變晚、不會提早一格 */
        self->rt_release = timer_wait_base() + period;
        rt_assign_levels();
        update_prio(self);
    }
    rt_check_accounting();
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

    KASSERT(self->pin_cpu == 0);             /* 正在執行的不會在固定佇列上 */
    self->state = T_ZOMBIE;
    c->zombie = self;                        /* 由這顆 CPU 上的下一條執行緒回收 */
    __atomic_fetch_add(&nzombies, 1, __ATOMIC_SEQ_CST);
    nthreads--;
    schedule();
    panic("thread_exit: zombie was scheduled again");
}

/*
 * 全系統只需要做一次的 tick 工作(喚醒休眠者、防飢餓檢查、即時工作的期限)。
 * 由「把系統時間從 from 推進到 to」的那顆 CPU 呼叫(任何一顆都可能,見 timer.c 的 timer_advance),
 * 呼叫時持有 time_lk、中斷關閉;一個 tick 只會有一顆 CPU 推進,所以每個 tick 恰好處理一次,
 * 不會重複喚醒(喚醒本身也在 sched_lk 之內從串列取下,天生只有一次)。
 * 逐個 tick 處理,與從前「第 0 顆 CPU 補記時每個 tick 呼叫一次」的語意相同(rt_check_deadlines
 * 每次只推進一個週期)。補記上限 REAL_CATCHUP_MAX(100)個 tick。
 */
void sched_tick_global(uint64_t from, uint64_t to)
{
    if (!this_cpu()->current)
        return;                              /* 排程器尚未啟動:沒有休眠者 */

    for (uint64_t now = from + 1; now <= to; now++) {
        spin_lock(&sched_lk);
        /* 喚醒時間到的休眠者。串列已依喚醒時間排序,所以只看最前面的;
           第一個還沒到期,後面的一定也還沒,可以直接結束 */
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
        spin_unlock(&sched_lk);
    }
}

/*
 * 每顆 CPU 自己的計時器中斷都會呼叫(中斷關閉中):這顆 CPU 自己的時間片與 CPU 時間。
 * 全系統的工作在 sched_tick_global(由推進系統時間的那顆 CPU 做)。
 */
void sched_tick(int charge)
{
    struct cpu *c = this_cpu();

    if (!c->current)
        return;                              /* 排程器尚未啟動 */

    /* 補記的 tick(中斷晚到的那段時間):時間已經在 timer.c 推進過了,但這顆 CPU 當時是被停住的,
       不是被目前這條執行緒用掉的 — 不記 CPU 時間、不扣時間片、不扣即時工作的預算。
       否則每次模擬器停頓,剛好在執行的那條執行緒就會被誤判為用了好幾毫秒(實測過:
       一個幾乎不做事的即時工作因此被判超出預算) */
    if (!charge)
        return;

    spin_lock(&sched_lk);

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
        if (highest_for(c->id) >= t->prio)
            c->need_resched = 1;
    }
    /* 閒置中的 CPU:佇列裡(包括別顆 CPU 的)有工作就去拿。平常有工作時會被直接通知,
       這裡是保險,確保不會有「有人在排隊、卻有 CPU 閒著」的情況持續超過一個 tick */
    if (t == c->idle && highest_for(c->id) >= 0)
        c->need_resched = 1;
    if (t != c->idle && t->slice > 0 && --t->slice == 0) {
        /* 靠防飢餓提升上來的執行緒只給一個時間片:用完就讓出,不論別人的優先權高低
           (它此刻是最高優先權,不這樣做的話會反過來霸佔 CPU) */
        int floor = t->starve_boost ? 0 : t->prio;
        if (highest_for(c->id) >= floor)
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

/* 還沒釋放的執行緒數 = 存活 + 已結束但還沒回收。thread_exit 在同一段 sched_lock 內
   把存活數減 1、待回收數加 1,所以在鎖內讀這個和,只有真正釋放(thread_free)時才會下降。
   測試用它確認「之前結束的執行緒資源都已歸還」,不必猜要等多久 */
int (*sched_sleep_check)(void *data);     /* 見 thread_sleep;NULL = 不檢查 */

unsigned sched_unfreed_threads(void)
{
    unsigned long flags = sched_lock();
    unsigned n = nthreads + __atomic_load_n(&nzombies, __ATOMIC_SEQ_CST);
    sched_unlock(flags);
    return n;
}
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
