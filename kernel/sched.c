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

#define STACK_PAGES   4u                     /* 每條執行緒的 stack:16KB */
#define STACK_SIZE    (STACK_PAGES * PAGE_SIZE)
#define TIME_SLICE    5u                     /* 時間片:5 個 tick(100Hz 時為 50ms) */
#define STACK_CANARY  0x57454C59u            /* "WELY":放在 stack 最底端的記號 */

enum thread_state {
    T_READY,       /* 在執行佇列中等待輪到 */
    T_RUNNING,     /* 正在使用 CPU */
    T_SLEEPING,    /* 等待時間到 */
    T_ZOMBIE       /* 已結束,等待別人回收它的 stack */
};

struct thread {
    void *sp;                  /* 被切換出去時的 stack pointer(arch_switch 存取) */
    int id;
    const char *name;
    enum thread_state state;
    char *stack;               /* stack 的最低位址;開機 stack 與此不同來源,為 NULL */
    uintptr_t stack_lo, stack_hi;
    void (*fn)(void *);        /* 進入點與參數 */
    void *arg;
    unsigned slice;            /* 剩餘的時間片(tick 數) */
    uint64_t wake_tick;        /* T_SLEEPING:到這個 tick 就喚醒 */
    struct thread *next;       /* 串在執行佇列或休眠串列中 */
};

/* 開機時 boot.S 設定的 stack,main 執行緒繼續使用它 */
extern char stack_bottom[], stack_top[];

static struct thread main_thread;            /* 開機流程本身 */
static struct thread *current;               /* 正在執行的執行緒;NULL = 排程器未啟動 */
static struct thread *idle_thread;           /* 沒人可跑時執行,不進執行佇列 */
static struct thread *rq_head, *rq_tail;     /* 執行佇列(FIFO):READY 的執行緒 */
static struct thread *sleepers;              /* 休眠中的執行緒(無順序) */
static struct thread *zombie;                /* 剛結束、等待回收的執行緒(最多一條) */
static volatile int need_resched;            /* 中斷中設定:返回前要重新排程 */
static unsigned nthreads;                    /* 存活數(不含 idle) */
static unsigned long nswitches;
static int next_id;

/* ---- 執行佇列:尾端加入、頭端取出,所以大家輪流 ---- */

static void rq_push(struct thread *t)
{
    t->state = T_READY;
    t->next = NULL;
    if (rq_tail)
        rq_tail->next = t;
    else
        rq_head = t;
    rq_tail = t;
}

static struct thread *rq_pop(void)
{
    struct thread *t = rq_head;
    if (t) {
        rq_head = t->next;
        if (!rq_head)
            rq_tail = NULL;
        t->next = NULL;
    }
    return t;
}

/*
 * 回收已結束的執行緒。執行緒不能釋放自己正踩著的 stack,
 * 所以由「切換過去的下一條執行緒」代為釋放。
 */
static void reap(void)
{
    struct thread *z = zombie;
    if (z && z != current) {
        zombie = NULL;
        pmm_free_pages(z->stack, STACK_PAGES);
        kfree(z);
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

    /* stack 往低位址成長,用過頭會先蓋掉最底端的記號 */
    if (prev->stack && *(uint32_t *)prev->stack != STACK_CANARY)
        panic("stack overflow in thread %d (%s)", prev->id, prev->name);

    if (prev->state == T_RUNNING && prev != idle_thread)
        rq_push(prev);

    struct thread *next = rq_pop();
    if (!next)
        next = idle_thread;                  /* 所有人都在睡:跑 idle 等中斷 */

    next->state = T_RUNNING;
    next->slice = TIME_SLICE;
    if (next == prev)
        return;                              /* 只有自己可跑,不必切換 */

    current = next;
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

static struct thread *thread_alloc(const char *name, void (*fn)(void *), void *arg)
{
    struct thread *t = kzalloc(sizeof(*t));
    if (!t)
        return NULL;
    t->stack = pmm_alloc_pages(STACK_PAGES);
    if (!t->stack) {
        kfree(t);
        return NULL;
    }
    *(uint32_t *)t->stack = STACK_CANARY;
    t->stack_lo = (uintptr_t)t->stack;
    t->stack_hi = t->stack_lo + STACK_SIZE;
    t->name = name;
    t->fn = fn;
    t->arg = arg;
    /* 在新 stack 上預先擺好一份「假的」已存暫存器,
       讓第一次 arch_switch 切過來時會「返回」到 thread_entry */
    t->sp = arch_stack_init((void *)t->stack_hi, thread_entry);
    return t;
}

void sched_init(void)
{
    main_thread.id = next_id++;
    main_thread.name = "main";
    main_thread.state = T_RUNNING;
    main_thread.slice = TIME_SLICE;
    main_thread.stack_lo = (uintptr_t)stack_bottom;
    main_thread.stack_hi = (uintptr_t)stack_top;
    nthreads = 1;

    idle_thread = thread_alloc("idle", idle_main, NULL);
    if (!idle_thread)
        panic("sched: cannot create idle thread");
    idle_thread->id = -1;

    current = &main_thread;                  /* 從這一刻起排程器算是啟動了 */
}

struct thread *thread_create(const char *name, void (*fn)(void *), void *arg)
{
    struct thread *t = thread_alloc(name, fn, arg);
    if (!t)
        return NULL;

    unsigned long flags = arch_irq_save();   /* 佇列也會被中斷路徑修改 */
    t->id = next_id++;
    nthreads++;
    rq_push(t);
    arch_irq_restore(flags);
    return t;
}

void thread_yield(void)
{
    unsigned long flags = arch_irq_save();
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
    current->next = sleepers;
    sleepers = current;
    schedule();                              /* 醒來後才會從這裡返回 */
    arch_irq_restore(flags);
}

void thread_exit(void)
{
    arch_irq_disable();
    if (current == &main_thread || current == idle_thread)
        panic("thread_exit: %s thread must not exit", current->name);
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

    /* 喚醒時間到的休眠者:從串列移除、放回執行佇列 */
    uint64_t now = timer_ticks();
    struct thread **pp = &sleepers;
    while (*pp) {
        struct thread *t = *pp;
        if (now >= t->wake_tick) {
            *pp = t->next;
            rq_push(t);
            /* 剛醒的人排到隊尾等待輪到,不插隊;只有 CPU 正閒著才立刻換人。
               (若一醒來就強制換人,正在跑的執行緒會被提早中斷而吃虧,見 DEVLOG) */
            if (current == idle_thread)
                need_resched = 1;
        } else {
            pp = &t->next;
        }
    }

    /* 時間片用完且有人在等,才需要換人 */
    if (current != idle_thread && current->slice > 0 && --current->slice == 0) {
        if (rq_head)
            need_resched = 1;
        else
            current->slice = TIME_SLICE;
    }
}

void sched_irq_exit(void)
{
    if (current && need_resched)
        schedule();
}

const char *thread_name(void)  { return current ? current->name : "boot"; }
int thread_id(void)            { return current ? current->id : 0; }
unsigned sched_thread_count(void)      { return nthreads; }
unsigned long sched_switch_count(void) { return nswitches; }

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
