/*
 * smp.c - 啟動其他 CPU(與架構無關的部分)
 */
#include <stddef.h>
#include "arch.h"
#include "cpu.h"
#include "kprintf.h"
#include "panic.h"
#include "pmm.h"
#include "ring.h"
#include "sched.h"
#include "smp.h"
#include "spinlock.h"
#include "timer.h"

_Static_assert(BOOT_MAX_CPUS == MAX_CPUS, "BOOT_MAX_CPUS");

#define AP_STACK_PAGES 4u                    /* 每顆 AP 的 stack:16KB */
#define START_TIMEOUT_MS 200u                /* 等一顆 CPU 回報的上限 */

static volatile int nstarted = 1;            /* 已啟動的 CPU 數(含開機的那一顆) */

/* AP 啟動後的階段,由開機的 CPU 設定、所有 AP 觀察 */
enum { PH_WAIT, PH_TEST, PH_STANDBY, PH_GO };
static volatile int phase = PH_WAIT;

/* ---- 處理器間中斷與跨 CPU 呼叫 ---- */

static struct spinlock call_lk = SPINLOCK_INIT("smp-call");   /* 同一時間只進行一個跨 CPU 呼叫 */
static void (*volatile call_fn)(void *);
static void *volatile call_arg;
static volatile int call_acks;               /* 已經執行完 call_fn 的 CPU 數 */

void smp_send(struct cpu *c, unsigned what)
{
    __atomic_fetch_or(&c->ipi_pending, what, __ATOMIC_SEQ_CST);  /* 先放內容,再叫它 */
    arch_ipi_send(c);
}

void smp_poll(void)
{
    struct cpu *c = this_cpu();

    if (!__atomic_load_n(&c->ipi_pending, __ATOMIC_RELAXED))
        return;                              /* 絕大多數時候沒有事:只花一次讀取 */
    unsigned p = __atomic_exchange_n(&c->ipi_pending, 0u, __ATOMIC_SEQ_CST);

    if (p & IPI_STOP)
        arch_smp_park();                     /* 不會返回 */
    if (p & IPI_CALL) {
        call_fn(call_arg);
        __atomic_fetch_add(&call_acks, 1, __ATOMIC_SEQ_CST);
    }
    /* IPI_RESCHED:不必做事。need_resched 已經設好,中斷返回前 sched_irq_exit 會處理 */
}

void smp_ipi_handler(void)
{
    smp_poll();
}

void smp_call_others(void (*fn)(void *), void *arg)
{
    if (cpu_count == 1)
        return;

    unsigned long flags = arch_irq_save();
    /* 別人也可能正要發起呼叫,而且正在等我們回應:搶不到鎖時要一邊處理它的要求 */
    while (!spin_trylock(&call_lk)) {
        smp_poll();
        arch_cpu_relax();
    }

    int me = this_cpu()->id, n = 0;
    call_fn = fn;
    call_arg = arg;
    __atomic_store_n(&call_acks, 0, __ATOMIC_SEQ_CST);
    for (int i = 0; i < cpu_count; i++) {
        if (i == me)
            continue;
        smp_send(&cpus[i], IPI_CALL);
        n++;
    }
    while (__atomic_load_n(&call_acks, __ATOMIC_SEQ_CST) != n) {
        smp_poll();                          /* 等待期間若系統 panic,自己也要能停下來 */
        arch_cpu_relax();
    }
    spin_unlock(&call_lk);
    arch_irq_restore(flags);
}

void smp_stop_others(void)
{
    if (nstarted == 1)
        return;
    int me = this_cpu()->id;
    for (int i = 0; i < nstarted; i++)
        if (i != me && cpus[i].started)
            smp_send(&cpus[i], IPI_STOP);
}

/* ---- 並行測試用的共用資料 ---- */
#define TEST_ROUNDS 20000u
#define TEST_ITEMS  (RING_SIZE - 1)
static struct spinlock test_lk = SPINLOCK_INIT("smp-test");
static volatile unsigned long locked_counter;        /* 持有鎖時遞增:不應該少 */
static volatile unsigned long racy_counter;          /* 不上鎖遞增:預期會少(示範用) */
static struct ring test_ring;                        /* 開機的 CPU 放入,所有 CPU 一起取 */
static volatile unsigned long ring_sum, ring_taken;
static volatile int test_done;                       /* 做完測試的 CPU 數 */
static volatile int id_errors;                       /* this_cpu() 不正確的次數 */
static volatile int traps_ok;                        /* 成功處理例外並返回的 AP 數 */

int smp_started(void)
{
    return nstarted;
}

/* 每顆 CPU 都執行同一份測試;me 是呼叫者認為自己應該是的 CPU */
static void run_test(struct cpu *me)
{
    /* 0. 例外處理:每顆 AP 自己觸發一次中斷點例外,確認它的例外入口、
          每顆 CPU 的指標與 stack 都設定正確(處理完會返回,繼續往下) */
    if (me->id != 0) {
        arch_test_breakpoint();
        __atomic_fetch_add(&traps_ok, 1, __ATOMIC_RELAXED);
    }

    /* 1. spinlock:所有 CPU 同時對同一個計數器加一。
          「讀出、加一、寫回」不是一個不可分割的動作,沒有鎖時兩顆 CPU 可能
          讀到同一個舊值,各自加一後寫回,結果只加了一次 */
    for (unsigned i = 0; i < TEST_ROUNDS; i++) {
        spin_lock(&test_lk);
        locked_counter++;
        spin_unlock(&test_lk);
        racy_counter++;
        /* 2. 每顆 CPU 的狀態:任何時候 this_cpu() 都必須是自己那一份 */
        if (this_cpu() != me)
            __atomic_fetch_add(&id_errors, 1, __ATOMIC_RELAXED);
    }

    /* 3. 無鎖環狀佇列:多顆 CPU 同時取出,每個項目只能被取走一次 */
    void *p;
    while ((p = ring_take(&test_ring)) != NULL) {
        __atomic_fetch_add(&ring_sum, (unsigned long)(uintptr_t)p, __ATOMIC_RELAXED);
        __atomic_fetch_add(&ring_taken, 1, __ATOMIC_RELAXED);
    }
    __atomic_fetch_add(&test_done, 1, __ATOMIC_SEQ_CST);
}

void smp_ap_main(void)
{
    struct cpu *c = this_cpu();              /* 到進入排程器之前,AP 不會換 CPU,可以放心使用 */

    arch_smp_cpu_init();                     /* 從這裡起可以接收處理器間中斷 */
    __atomic_store_n(&c->started, 1, __ATOMIC_SEQ_CST);

    /* 等開機的 CPU 決定要不要先跑並行測試。這段等待很短(所有 CPU 都啟動後就會有結果) */
    int ph;
    while ((ph = __atomic_load_n(&phase, __ATOMIC_ACQUIRE)) == PH_WAIT)
        arch_cpu_relax();
    if (ph == PH_TEST)
        run_test(c);

    /* 待命:開著中斷休眠,不佔用主機的運算資源。開機的 CPU 呼叫 smp_sched_start()
       時會發出處理器間中斷把我們叫醒 */
    arch_irq_enable();
    while (__atomic_load_n(&phase, __ATOMIC_ACQUIRE) != PH_GO)
        arch_idle();
    arch_irq_disable();

    /* 加入排程:目前這段執行流程就成為這顆 CPU 的 idle 執行緒 */
    sched_init_ap();
    arch_timer_init_cpu();                   /* 每顆 CPU 自己的計時器:時間片與搶佔靠它 */
    arch_irq_enable();
    for (;;)
        arch_idle();                         /* 有工作時由中斷返回的路徑切換過去 */
}

/* 等條件成立或逾時。成立回傳 1 */
static int wait_for(volatile int *v, int want, unsigned ms)
{
    uint64_t t0 = timer_ticks();
    unsigned limit = ms * timer_hz() / 1000 + 1;

    while (__atomic_load_n(v, __ATOMIC_ACQUIRE) != want) {
        if (timer_ticks() - t0 > limit)
            return 0;
        arch_cpu_relax();
    }
    return 1;
}

void smp_init(const struct boot_info *bi, int run)
{
    uint64_t ids[MAX_CPUS - 1];
    int n = arch_smp_discover(bi, ids, MAX_CPUS - 1);

    for (int i = 0; i < n; i++) {
        struct cpu *c = &cpus[nstarted];

        /* 每顆 CPU 要有自己的 stack:它一開始執行 C 程式就會用到 */
        char *stack = pmm_alloc_pages(AP_STACK_PAGES);
        if (!stack) {
            kprintf("smp: out of memory, cpu with hardware id %llu not started\n",
                    (unsigned long long)ids[i]);
            break;
        }
        c->id = nstarted;
        c->arch_id = ids[i];
        c->kstack_limit = (uintptr_t)stack;
        c->kstack_top = (uintptr_t)stack + AP_STACK_PAGES * PAGE_SIZE;
        c->started = 0;

        /* 一次只啟動一顆,等它回報後再啟動下一顆:啟動流程中有些資料
           (例如 i386 的啟動參數)是所有 AP 共用的,同時啟動會互相覆蓋 */
        if (arch_smp_start(c) != 0 || !wait_for(&c->started, 1, START_TIMEOUT_MS)) {
            kprintf("smp: cpu with hardware id %llu did not start\n",
                    (unsigned long long)ids[i]);
            /* stack 不釋放:那顆 CPU 也許只是慢了,之後才開始用它 */
            continue;
        }
        nstarted++;
    }

    arch_smp_done();

    if (nstarted == 1) {
        kprintf("smp: 1 cpu\n");
        return;
    }
    kprintf("smp: %d cpus started (hardware ids:", nstarted);
    for (int i = 0; i < nstarted; i++)
        kprintf(" %llu", (unsigned long long)cpus[i].arch_id);
    kprintf(")\n");

    if (!run) {
        __atomic_store_n(&phase, PH_STANDBY, __ATOMIC_RELEASE);
        return;
    }

    /* 並行測試:開機的 CPU 先把項目放進環(只有擁有者能放入),再放行大家一起跑 */
    unsigned long expect_sum = 0;
    for (unsigned long v = 1; v <= TEST_ITEMS; v++) {
        KASSERT(ring_push(&test_ring, (void *)(uintptr_t)v));
        expect_sum += v;
    }
    __atomic_store_n(&phase, PH_TEST, __ATOMIC_RELEASE);

    /* 自己也參與。關中斷:測試裡直接使用 spin_lock 與 this_cpu() */
    unsigned long flags = arch_irq_save();
    run_test(this_cpu());
    arch_irq_restore(flags);

    if (!wait_for(&test_done, nstarted, 2000))
        panic("smp: only %d of %d cpus finished the test", test_done, nstarted);

    unsigned long want = (unsigned long)nstarted * TEST_ROUNDS;
    kprintf("smp: %d cpus x %u increments: with spinlock %lu (expected %lu), "
            "without %lu (lost %lu)\n", nstarted, TEST_ROUNDS, locked_counter, want,
            racy_counter, want - racy_counter);
    kprintf("smp: lock-free ring: %lu items taken by %d cpus, sum %lu (expected %lu)\n",
            ring_taken, nstarted, ring_sum, expect_sum);
    KASSERT(locked_counter == want);
    KASSERT(ring_taken == TEST_ITEMS && ring_sum == expect_sum);
    KASSERT(id_errors == 0);
    KASSERT(traps_ok == nstarted - 1);
    kprintf("smp: selftest ok\n");
}

void smp_sched_start(void)
{
    if (nstarted == 1 || phase == PH_GO)
        return;

    __atomic_store_n(&phase, PH_GO, __ATOMIC_RELEASE);

    /* 叫醒待命中的 AP,等它們都登記進排程器。AP 可能剛好在「檢查完、正要休眠」的
       瞬間錯過這次中斷,所以每隔一個 tick 對還沒就緒的再叫一次 */
    for (unsigned tries = 0; ; tries++) {
        int ready = 1;
        for (int i = 1; i < nstarted; i++) {
            if (!cpus[i].online) {
                ready = 0;
                smp_send(&cpus[i], IPI_RESCHED);
            }
        }
        if (ready)
            break;
        if (tries > 1000)
            panic("smp: secondary cpus did not join the scheduler");
        thread_sleep(1);
    }

    /* 從這一行起,排程器與執行佇列才把其他 CPU 算進來 */
    sched_set_cpu_count(nstarted);
    kprintf("smp: %d cpus scheduling\n", nstarted);
}
