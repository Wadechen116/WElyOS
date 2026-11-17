/*
 * sched.c - 核心執行緒與 round-robin 排程器(與架構無關)
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
 * 規則:schedule() 一律在「中斷關閉」的狀態下執行;切回來的執行緒
 * 各自負責還原自己的中斷狀態(主動的由 arch_irq_restore,搶佔的由中斷返回指令)。
 */
#include <stddef.h>
#include "arch.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "panic.h"
#include "pmm.h"
#include "sched.h"
#include "timer.h"
#include "vm.h"

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
    uint64_t wake_tick;        /* T_SLEEPING:到這個 tick 就喚醒 */
    struct thread *next;       /* 串在執行佇列、休眠串列或某個等待佇列中(同時只會在一個) */
    struct thread *all_next;   /* 串在「所有執行緒」串列中,供 thread_join 以編號尋找 */
    struct vm_space *space;    /* 使用的位址空間;NULL = 純 kernel 執行緒 */
    void *data;                /* 附掛資料(所屬的行程) */
    volatile int killed;       /* 被要求終止:返回使用者模式前自行結束 */
    struct wait_queue joiners; /* 正在 thread_join 等我結束的執行緒 */
};

/* 開機時 boot.S 設定的 stack,main 執行緒繼續使用它;stack_guard 是它下方的保護頁 */
extern char stack_guard[], stack_bottom[], stack_top[];

uintptr_t kstack_limit;                      /* 見 sched.h */

static struct thread main_thread;            /* 開機流程本身 */
static struct thread *current;               /* 正在執行的執行緒;NULL = 排程器未啟動 */
static struct thread *idle_thread;           /* 沒人可跑時執行,不進執行佇列 */
/*
 * 執行佇列:每個優先權一條 FIFO 串列,另用一個位元圖記錄哪些佇列非空。
 * 選下一條執行緒 = 找位元圖中最高的 1,再取那條佇列的頭,
 * 成本與執行緒總數無關(只和優先權的級數有關,固定 8 級)。
 */
static struct thread *rq_head[PRIO_LEVELS], *rq_tail[PRIO_LEVELS];
static unsigned rq_bitmap;                   /* bit p = 1:優先權 p 的佇列非空 */
static int in_irq;                           /* 是否正在中斷處理程式中 */
static struct vm_space *loaded_space;        /* CPU 目前載入的位址空間;NULL = kernel 頁表 */
static struct thread *sleepers;              /* 休眠中的執行緒,依喚醒時間由早到晚排序 */
static struct thread *all_threads;           /* 所有存活的執行緒(不含 main 與 idle) */
static struct thread *zombie;                /* 剛結束、等待回收的執行緒(最多一條) */
static volatile int need_resched;            /* 中斷中設定:返回前要重新排程 */
static unsigned nthreads;                    /* 存活數(不含 idle) */
static unsigned long nswitches;
static int next_id;
static unsigned time_slice = 1;              /* 時間片換算成 tick 數,sched_init 依計時器頻率設定 */
static unsigned starve_ticks = 100;          /* STARVE_MS 換算成 tick 數 */
static unsigned starve_scan_ticks = 10;      /* STARVE_SCAN_MS 換算成 tick 數 */
static unsigned long nstarve_boosts;         /* 累計的防飢餓提升次數 */

static void schedule(void);

/* ---- 執行佇列:尾端加入、頭端取出,所以大家輪流 ---- */

/* 排到自己優先權那條佇列的尾端 */
static void rq_push(struct thread *t)
{
    int p = t->prio;

    t->ready_since = timer_ticks();          /* 從現在開始算等待時間 */
    t->state = T_READY;
    t->next = NULL;
    if (rq_tail[p])
        rq_tail[p]->next = t;
    else
        rq_head[p] = t;
    rq_tail[p] = t;
    rq_bitmap |= 1u << p;
}

/* 排到佇列的最前面:被更高優先權搶佔的執行緒,時間片還沒用完,
   不該因此排到同級的最後面 */
static void rq_push_front(struct thread *t)
{
    int p = t->prio;

    t->state = T_READY;
    t->next = rq_head[p];
    rq_head[p] = t;
    if (!rq_tail[p])
        rq_tail[p] = t;
    rq_bitmap |= 1u << p;
}

/* 目前可執行的最高優先權;沒有任何可執行的執行緒回傳 -1 */
static int rq_highest(void)
{
    for (int p = PRIO_MAX; p >= PRIO_MIN; p--)
        if (rq_bitmap & (1u << p))
            return p;
    return -1;
}

/* 取出優先權最高的那條佇列的頭 */
static struct thread *rq_pop(void)
{
    int p = rq_highest();
    if (p < 0)
        return NULL;

    struct thread *t = rq_head[p];
    rq_head[p] = t->next;
    if (!rq_head[p]) {
        rq_tail[p] = NULL;
        rq_bitmap &= ~(1u << p);
    }
    t->next = NULL;
    return t;
}

/* 把一條 READY 的執行緒從它的佇列中移除(改優先權時用;需要走訪該佇列) */
static void rq_remove(struct thread *t)
{
    int p = t->prio;
    struct thread *prev = NULL;

    for (struct thread *c = rq_head[p]; c; prev = c, c = c->next) {
        if (c != t)
            continue;
        if (prev)
            prev->next = t->next;
        else
            rq_head[p] = t->next;
        if (rq_tail[p] == t)
            rq_tail[p] = prev;
        if (!rq_head[p])
            rq_bitmap &= ~(1u << p);
        t->next = NULL;
        return;
    }
}

/* 一條執行緒剛變成可執行:若它比目前的更優先(或 CPU 正閒著),記下需要重新排程 */
static void make_ready(struct thread *t)
{
    rq_push(t);
    if (current == idle_thread || t->prio > current->prio)
        need_resched = 1;
}

/* ---- 等待佇列 ---- */

void wq_sleep(struct wait_queue *wq)
{
    struct thread *t = current;

    t->state = T_BLOCKED;                    /* 不是 RUNNING,schedule() 就不會把它排回去 */
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
    unsigned long flags = arch_irq_save();

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
        make_ready(best);
    }
    arch_irq_restore(flags);
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
 * 回收已結束的執行緒。執行緒不能釋放自己正踩著的 stack,
 * 所以由「切換過去的下一條執行緒」代為釋放。
 */
static void stack_free(char *stack);

static void reap(void)
{
    struct thread *z = zombie;
    if (z && z != current) {
        zombie = NULL;
        stack_free(z->stack);
        kfree(z);
    }
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
static char *stack_cache;                    /* 閒置 stack 的串列(存的是配置區塊起點) */
static unsigned stack_cached;

static char *stack_alloc(void)
{
    unsigned long flags = arch_irq_save();
    char *st = stack_cache;
    if (st) {
        stack_cache = *(char **)(st + PAGE_SIZE);
        stack_cached--;
    }
    arch_irq_restore(flags);
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

/* 把一條 stack 真正還給 pmm:保護頁要先對映回來,否則下一個拿到這頁的人無法使用 */
static void stack_release(char *st)
{
    if (vm_map((uintptr_t)st, (uintptr_t)st, VM_READ | VM_WRITE) != 0)
        panic("sched: cannot remap guard page %p", (void *)st);
    pmm_free_pages(st, STACK_PAGES + 1);
}

static void stack_free(char *st)
{
    unsigned long flags = arch_irq_save();
    if (stack_cached < STACK_CACHE_MAX) {
        *(char **)(st + PAGE_SIZE) = stack_cache;
        stack_cache = st;
        stack_cached++;
        st = NULL;
    }
    arch_irq_restore(flags);
    if (st)
        stack_release(st);
}

void sched_trim(void)
{
    for (;;) {
        unsigned long flags = arch_irq_save();
        char *st = stack_cache;
        if (st) {
            stack_cache = *(char **)(st + PAGE_SIZE);
            stack_cached--;
        }
        arch_irq_restore(flags);
        if (!st)
            break;
        stack_release(st);
    }
}

/*
 * 排程:選出下一條執行緒並切換過去。呼叫前必須已關閉中斷。
 * 目前的執行緒若還能跑(RUNNING)就排回隊尾;若已改成 SLEEPING / ZOMBIE 則不排。
 */
static void schedule(void)
{
    struct thread *prev = current;

    need_resched = 0;

    /* 防飢餓的提升只給一次上 CPU 的機會:離開 CPU 時就收回 */
    if (prev->starve_boost) {
        prev->starve_boost = 0;
        prev->prio = prev->inherit_prio > prev->base_prio ? prev->inherit_prio
                                                           : prev->base_prio;
    }

    if (prev->state == T_RUNNING && prev != idle_thread) {
        if (prev->slice > 0)
            rq_push_front(prev);             /* 被更優先的搶佔:保留位置與剩餘時間片 */
        else
            rq_push(prev);                   /* 時間片用完或主動讓出:排到同級最後 */
    }

    struct thread *next = rq_pop();          /* 優先權最高的;同級則是排最久的 */
    if (!next)
        next = idle_thread;                  /* 所有人都在睡:跑 idle 等中斷 */

    next->state = T_RUNNING;
    if (next->slice == 0)
        next->slice = time_slice;            /* 新的一輪;被搶佔回來的沿用剩餘的 */
    if (next == prev)
        return;                              /* 仍然是自己最優先,不必切換 */

    current = next;
    kstack_limit = next->stack_lo;           /* 例外入口用它檢查 stack 是否溢位 */
    arch_set_kstack(next->stack_hi);         /* 從使用者模式進入 kernel 時要用的 stack */
    /* 換頁表很貴(TLB 會被清空),所以只在必要時才換:kernel 執行緒不需要
       特定的位址空間,繼續用上一個行程的頁表即可(kernel 部分完全相同) */
    if (next->space && next->space != loaded_space) {
        vm_space_switch(next->space);
        loaded_space = next->space;
    }
    nswitches++;
    arch_switch(&prev->sp, next->sp);

    /* 執行到這裡時已經是「之後某次被切回來」;此刻 current == prev */
    reap();
}

/* 新執行緒第一次被切換到時從這裡開始(由 arch_stack_init 安排) */
static void thread_entry(void)
{
    reap();                                  /* 上一條若剛結束,替它收尾 */
    arch_irq_enable();                       /* schedule() 是關著中斷切過來的 */
    current->fn(current->arg);
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
    struct thread *t = kzalloc(sizeof(*t));
    if (!t)
        return NULL;
    t->stack = stack_alloc();
    if (!t->stack) {
        kfree(t);
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
    kstack_limit = main_thread.stack_lo;

    idle_thread = thread_alloc("idle", idle_main, NULL, PRIO_MIN);
    if (!idle_thread)
        panic("sched: cannot create idle thread");
    idle_thread->id = -1;

    current = &main_thread;                  /* 從這一刻起排程器算是啟動了 */
}

int thread_create(const char *name, void (*fn)(void *), void *arg)
{
    return thread_create_prio(name, fn, arg, PRIO_NORMAL);
}

int thread_create_prio(const char *name, void (*fn)(void *), void *arg, int prio)
{
    if (prio < PRIO_MIN || prio > PRIO_MAX)
        return -1;

    struct thread *t = thread_alloc(name, fn, arg, prio);
    if (!t)
        return -1;

    unsigned long flags = arch_irq_save();   /* 佇列也會被中斷路徑修改 */
    int id = t->id = next_id++;
    nthreads++;
    t->all_next = all_threads;
    all_threads = t;
    make_ready(t);
    sched_preempt_point();                   /* 新執行緒比自己優先:立刻讓它跑 */
    arch_irq_restore(flags);
    /* 注意:開中斷之後 t 隨時可能已經跑完並被釋放,所以回傳先記下的 id,不再碰 t */
    return id;
}

void thread_join(int tid)
{
    unsigned long flags = arch_irq_save();

    /* 「尋找」與「睡下去」之間中斷一直是關的,對方不可能剛好在中間結束 */
    for (struct thread *t = all_threads; t; t = t->all_next) {
        if (t->id == tid) {
            KASSERT(t != current);           /* 等自己結束會永遠等下去 */
            wq_sleep(&t->joiners);
            break;                           /* 被喚醒時 t 已結束,不可再存取 */
        }
    }
    arch_irq_restore(flags);
}

void thread_set_space(struct vm_space *vs)
{
    unsigned long flags = arch_irq_save();
    current->space = vs;
    vm_space_switch(vs);
    loaded_space = vs;
    arch_irq_restore(flags);
}

void thread_set_data(void *data) { current->data = data; }
void *thread_data(void)          { return current ? current->data : NULL; }
int thread_killed(void)          { return current && current->killed; }

void sched_kill_group(void *data)
{
    unsigned long flags = arch_irq_save();
    for (struct thread *t = all_threads; t; t = t->all_next)
        if (t->data == data)
            t->killed = 1;
    arch_irq_restore(flags);
}

void sched_preempt_point(void)
{
    if (current && need_resched && !in_irq)
        schedule();
}

/*
 * 重新計算 t 的有效優先權並維持各佇列的一致。呼叫時中斷已關閉。
 *   有效優先權 = 正處於防飢餓提升 ? 最高 : max(自己的, 繼承來的)
 */
static void update_prio(struct thread *t)
{
    int prio = t->inherit_prio > t->base_prio ? t->inherit_prio : t->base_prio;
    if (t->starve_boost)
        prio = PRIO_MAX;
    if (t->prio == prio)
        return;

    if (t->state == T_READY) {
        uint64_t since = t->ready_since;
        rq_remove(t);                        /* 換到新優先權的佇列 */
        t->prio = prio;
        make_ready(t);
        t->ready_since = since;              /* 換佇列不算重新開始等 */
    } else {
        t->prio = prio;                      /* RUNNING / SLEEPING / BLOCKED:改欄位即可 */
        /* 自己調低後,若有人比自己優先,要讓出 */
        if (t == current && rq_highest() > prio)
            need_resched = 1;
    }
}

void thread_set_priority(int prio)
{
    if (prio < PRIO_MIN || prio > PRIO_MAX)
        return;

    unsigned long flags = arch_irq_save();
    current->base_prio = prio;
    update_prio(current);
    sched_preempt_point();
    arch_irq_restore(flags);
}

int thread_priority(void)
{
    return current ? current->prio : PRIO_NORMAL;
}

int thread_effective_prio(struct thread *t) { return t->prio; }
void *thread_blocked_on(struct thread *t)   { return t->blocked_on; }
void thread_set_blocked_on(void *m)         { current->blocked_on = m; }
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
 */
static void starve_scan(uint64_t now)
{
    for (int p = PRIO_MAX - 1; p >= PRIO_MIN; p--) {
        struct thread *t = rq_head[p];
        if (!t || now - t->ready_since < starve_ticks)
            continue;
        t->starve_boost = 1;
        nstarve_boosts++;
        update_prio(t);                      /* 移到最高優先權的佇列;必要時要求重新排程 */
    }
}

void thread_yield(void)
{
    unsigned long flags = arch_irq_save();
    current->slice = 0;                      /* 主動放棄剩餘的時間片,排到同級最後 */
    schedule();
    arch_irq_restore(flags);
}

void thread_sleep(unsigned ms)
{
    /* 毫秒換算成 tick,無條件進位;再加 1 是因為目前這個 tick 已經過了一部分。
       刻意用 32 位元運算:i386 上 64 位元除法需要 libgcc 的 __udivdi3,
       而 freestanding 環境沒有連結它(ms 上限因此約為 4 千萬毫秒,足夠) */
    unsigned n = (ms * timer_hz() + 999) / 1000 + 1;

    unsigned long flags = arch_irq_save();
    current->wake_tick = timer_ticks() + n;
    current->state = T_SLEEPING;

    /* 依喚醒時間插入到正確位置:每個 tick 只需檢查串列的第一個。
       用 <= 讓同一時間到期的維持先來後到的順序 */
    struct thread **pp = &sleepers;
    while (*pp && (*pp)->wake_tick <= current->wake_tick)
        pp = &(*pp)->next;
    current->next = *pp;
    *pp = current;

    schedule();                              /* 醒來後才會從這裡返回 */
    arch_irq_restore(flags);
}

void thread_exit(void)
{
    arch_irq_disable();
    if (current == &main_thread || current == idle_thread)
        panic("thread_exit: %s thread must not exit", current->name);
    /* 從「所有執行緒」串列移除:之後的 thread_join 找不到它,會立即返回 */
    for (struct thread **pp = &all_threads; *pp; pp = &(*pp)->all_next) {
        if (*pp == current) {
            *pp = current->all_next;
            break;
        }
    }
    wq_wake_all(&current->joiners);          /* 叫醒所有在等我結束的執行緒 */

    current->state = T_ZOMBIE;
    zombie = current;                        /* 由下一條執行緒 reap() */
    nthreads--;
    schedule();
    panic("thread_exit: zombie was scheduled again");
}

void sched_tick(void)
{
    if (!current)
        return;                              /* 排程器尚未啟動 */

    /* 喚醒時間到的休眠者。串列已依喚醒時間排序,所以只看最前面的;
       第一個還沒到期,後面的一定也還沒,可以直接結束 */
    uint64_t now = timer_ticks();
    while (sleepers && now >= sleepers->wake_tick) {
        struct thread *t = sleepers;
        sleepers = t->next;
        /* 相同優先權:排到隊尾等待輪到,不插隊(見 DEVLOG 第 10 步的公平性問題)。
           比目前的更優先:make_ready 會要求立刻重新排程 */
        make_ready(t);
    }

    /* 定期檢查有沒有人等太久(防飢餓) */
    /* 轉成 32 位元再取餘數:i386 上 64 位元的除法 / 餘數需要 libgcc */
    if ((unsigned)now % starve_scan_ticks == 0)
        starve_scan(now);

    /* 時間片用完:有「優先權不低於自己」的人在等才換人。
       只有較低優先權的在等時不換,它們本來就輪不到 */
    if (current != idle_thread && current->slice > 0 && --current->slice == 0) {
        if (rq_highest() >= current->prio)
            need_resched = 1;
        else
            current->slice = time_slice;
    }
}

void sched_irq_enter(void)
{
    in_irq = 1;
}

void sched_irq_exit(void)
{
    in_irq = 0;
    if (current && need_resched)
        schedule();
}

struct thread *thread_current(void) { return current; }
const char *thread_name(void)  { return current ? current->name : "boot"; }
int thread_id(void)            { return current ? current->id : 0; }
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
    if (current) {
        *lo = current->stack_lo;
        *hi = current->stack_hi;
    } else {
        *lo = (uintptr_t)stack_bottom;
        *hi = (uintptr_t)stack_top;
    }
}
