/*
 * main.c - kernel 的 C 語言進入點(與架構無關)
 *
 * 由各架構的 arch_start()(arch/<名稱>/start.c)在完成最基本的
 * 硬體初始化、並把開機資訊轉成 struct boot_info 後呼叫。
 */
#include <stdint.h>
#include "arch.h"
#include "boot_info.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "kstring.h"
#include "panic.h"
#include "pmm.h"
#include "sched.h"
#include "sync.h"
#include "timer.h"

/*
 * 開機參數(cmdline)中是否含有指定的「完整單字」(以空白分隔)。
 * 例:cmdline = "kernel.elf autoexit",word = "autoexit" -> 1。
 * 整字比對可避免 "autoexit2" 誤判。
 */
static int cmdline_has(const char *cmd, const char *word)
{
    while (*cmd) {
        while (*cmd == ' ')                  /* 跳過前導空白 */
            cmd++;
        const char *a = cmd, *b = word;
        while (*a && *a != ' ' && *a == *b) {
            a++;
            b++;
        }
        /* 比對到 word 結尾,且 cmdline 該單字也剛好結束 -> 整字相符 */
        if ((*a == '\0' || *a == ' ') && *b == '\0')
            return 1;
        while (*cmd && *cmd != ' ')          /* 不符:跳到下一個單字 */
            cmd++;
    }
    return 0;
}

/*
 * 測試用:由開機參數 crash=<種類> 故意觸發錯誤,驗證例外與 panic 處理。
 *   crash=panic    直接呼叫 panic()
 *   crash=fault    觸發記憶體/保護錯誤(方式依架構而定,見 arch_test_fault)
 *   crash=illegal  執行非法指令
 *   crash=break    執行軟體中斷點指令(可恢復的例外,處理後會繼續往下)
 *   crash=dfree    同一塊記憶體釋放兩次(驗證 kfree 的檢查)
 * (crash=stack、crash=thread、crash=relock 需要排程器,在 sched_demo 之前另外處理)
 */
static void crash_test(const char *cmd)
{
    if (cmdline_has(cmd, "crash=panic")) {
        panic("test panic, value=%d", 42);
    } else if (cmdline_has(cmd, "crash=fault")) {
        kprintf("triggering fault...\n");
        arch_test_fault();
        kprintf("fault did not trap!\n");       /* 不應到達這裡 */
    } else if (cmdline_has(cmd, "crash=illegal")) {
        kprintf("executing illegal instruction...\n");
        arch_test_illegal();
    } else if (cmdline_has(cmd, "crash=break")) {
        kprintf("executing breakpoint instruction...\n");
        arch_test_breakpoint();
        kprintf("survived breakpoint\n");
    } else if (cmdline_has(cmd, "crash=dfree")) {
        void *p = kmalloc(32);
        kprintf("freeing %p twice...\n", p);
        kfree(p);
        kfree(p);
    }
}

/* ------------------------------------------------------------------ */
/* 排程器示範與測試                                                    */
/* ------------------------------------------------------------------ */

/* 等待一組執行緒全部結束。thread_join 會睡到對方結束時被直接喚醒,不必輪詢 */
static void join_all(const int *tids, int n)
{
    for (int i = 0; i < n; i++) {
        KASSERT(tids[i] > 0);                /* 建立必須成功 */
        thread_join(tids[i]);
    }
}

/* 示範 1:主動讓出。每印一行就 yield,三條執行緒應該整齊地輪流 */
static void coop_worker(void *arg)
{
    for (int i = 1; i <= 3; i++) {
        kprintf("  [%s] step %d\n", thread_name(), i);
        thread_yield();
    }
    (void)arg;
}

/* 示範 2:搶佔。完全不讓出 CPU 的忙碌迴圈,只能靠計時器中斷換人 */
static volatile uint64_t spin_deadline;

static void spin_worker(void *arg)
{
    volatile unsigned long *counter = arg;
    while (timer_ticks() < spin_deadline)
        (*counter)++;
}

/* 示範 3:休眠。各自睡不同長度,醒來的順序應該依時間而不是建立順序 */
static void sleep_worker(void *arg)
{
    unsigned ms = (unsigned)(uintptr_t)arg;
    thread_sleep(ms);
    kprintf("  [%s] woke after %u ms (tick %llu)\n", thread_name(), ms,
            (unsigned long long)timer_ticks());
}

static void sched_demo(void)
{
    size_t free0 = pmm_free_count();
    struct kheap_stats h0, h1;
    int t[3];
    kheap_stats(&h0);

    kprintf("sched: cooperative (yield)\n");
    t[0] = thread_create("A", coop_worker, NULL);
    t[1] = thread_create("B", coop_worker, NULL);
    t[2] = thread_create("C", coop_worker, NULL);
    join_all(t, 3);

    kprintf("sched: preemptive (busy loops, no yield)\n");
    static volatile unsigned long c1, c2;
    unsigned long sw0 = sched_switch_count();
    spin_deadline = timer_ticks() + 50;      /* 兩條各自空轉到 0.5 秒後 */
    t[0] = thread_create("spin1", spin_worker, (void *)&c1);
    t[1] = thread_create("spin2", spin_worker, (void *)&c2);
    join_all(t, 2);
    kprintf("  spin1=%lu spin2=%lu, %lu context switches\n",
            (unsigned long)c1, (unsigned long)c2, sched_switch_count() - sw0);
    /* 兩條都要跑到,而且沒有一條被餓死(差距在 3 倍以內) */
    KASSERT(c1 > 0 && c2 > 0 && c1 < c2 * 3 && c2 < c1 * 3);

    kprintf("sched: sleep\n");
    t[0] = thread_create("s300", sleep_worker, (void *)300);
    t[1] = thread_create("s100", sleep_worker, (void *)100);
    t[2] = thread_create("s200", sleep_worker, (void *)200);
    join_all(t, 3);

    /* 所有示範執行緒都結束後,它們的 stack 與結構都應該已歸還 */
    kheap_stats(&h1);
    KASSERT(sched_thread_count() == 1);
    KASSERT(pmm_free_count() == free0);
    KASSERT(h1.free_bytes == h0.free_bytes);
    kprintf("sched: all threads exited, memory reclaimed\n");
}

/* ------------------------------------------------------------------ */
/* 同步機制示範                                                        */
/* ------------------------------------------------------------------ */

/*
 * 示範 1:競爭條件(race condition)與 mutex。
 * 「讀出 -> 加 1 -> 寫回」如果中途被別的執行緒插入,更新就會遺失。
 * 這裡在讀與寫之間刻意 thread_yield(),讓問題每次都必然發生,方便觀察;
 * 實際程式中它是由搶佔偶爾造成的,所以更難發現。
 */
#define RACE_THREADS 3
#define RACE_ROUNDS  100

static volatile int race_counter;
static struct mutex race_mutex = MUTEX_INIT;

static void race_worker(void *arg)
{
    int use_mutex = (int)(uintptr_t)arg;

    for (int i = 0; i < RACE_ROUNDS; i++) {
        if (use_mutex)
            mutex_lock(&race_mutex);
        int tmp = race_counter;              /* 讀出 */
        thread_yield();                      /* 這時別人可能也讀到同一個舊值 */
        race_counter = tmp + 1;              /* 寫回:蓋掉別人在這期間的更新 */
        if (use_mutex)
            mutex_unlock(&race_mutex);
    }
}

static int race_run(int use_mutex)
{
    int t[RACE_THREADS];

    race_counter = 0;
    for (int i = 0; i < RACE_THREADS; i++)
        t[i] = thread_create("race", race_worker, (void *)(uintptr_t)use_mutex);
    join_all(t, RACE_THREADS);
    return race_counter;
}

/*
 * 示範 2:生產者 / 消費者(bounded buffer),號誌的經典用法。
 *   slots:緩衝區還有幾個空位。生產者放入前 wait,滿了就睡
 *   items:緩衝區有幾個項目。  消費者取出前 wait,空了就睡
 *   mutex:保護緩衝區本身的索引
 * 緩衝區只有 4 格,生產者要放 20 個,所以一定會多次因為滿而睡著,
 * 由消費者取走後喚醒。
 */
#define PC_SLOTS     4
#define PC_ITEMS     20
#define PC_CONSUMERS 2

static int pc_buf[PC_SLOTS];
static unsigned pc_head, pc_tail;
static struct semaphore pc_slots, pc_items;
static struct mutex pc_mutex = MUTEX_INIT;
static int pc_sum, pc_count[PC_CONSUMERS];

static void pc_put(int v)
{
    sem_wait(&pc_slots);                     /* 等到有空位 */
    mutex_lock(&pc_mutex);
    pc_buf[pc_tail++ % PC_SLOTS] = v;
    mutex_unlock(&pc_mutex);
    sem_post(&pc_items);                     /* 通知:多了一個項目 */
}

static int pc_get(void)
{
    sem_wait(&pc_items);                     /* 等到有項目 */
    mutex_lock(&pc_mutex);
    int v = pc_buf[pc_head++ % PC_SLOTS];
    mutex_unlock(&pc_mutex);
    sem_post(&pc_slots);                     /* 通知:多了一個空位 */
    return v;
}

static void producer(void *arg)
{
    (void)arg;
    for (int i = 1; i <= PC_ITEMS; i++)
        pc_put(i);
    for (int i = 0; i < PC_CONSUMERS; i++)
        pc_put(-1);                          /* 每個消費者一個結束記號 */
}

static void consumer(void *arg)
{
    int id = (int)(uintptr_t)arg;
    int v;

    while ((v = pc_get()) != -1) {
        mutex_lock(&pc_mutex);
        pc_sum += v;
        pc_count[id]++;
        mutex_unlock(&pc_mutex);
        thread_yield();                      /* 讓另一個消費者也有機會拿到 */
    }
}

static void join_sleeper(void *arg)
{
    (void)arg;
    thread_sleep(100);
}

static void sync_demo(void)
{
    int t[1 + PC_CONSUMERS];

    kprintf("sync: race condition, %d threads x %d increments\n",
            RACE_THREADS, RACE_ROUNDS);
    int unsafe = race_run(0);
    int safe = race_run(1);
    kprintf("  without mutex: %d    with mutex: %d    (expected %d)\n",
            unsafe, safe, RACE_THREADS * RACE_ROUNDS);
    KASSERT(unsafe < RACE_THREADS * RACE_ROUNDS);    /* 沒保護:一定有遺失 */
    KASSERT(safe == RACE_THREADS * RACE_ROUNDS);     /* 有保護:一個都不能少 */

    kprintf("sync: producer/consumer, %d items through a %d-slot buffer\n",
            PC_ITEMS, PC_SLOTS);
    sem_init(&pc_slots, PC_SLOTS);
    sem_init(&pc_items, 0);
    t[0] = thread_create("producer", producer, NULL);
    t[1] = thread_create("consumer0", consumer, (void *)0);
    t[2] = thread_create("consumer1", consumer, (void *)1);
    join_all(t, 3);
    kprintf("  consumed %d + %d items, sum=%d (expected %d)\n",
            pc_count[0], pc_count[1], pc_sum, PC_ITEMS * (PC_ITEMS + 1) / 2);
    KASSERT(pc_count[0] + pc_count[1] == PC_ITEMS);
    KASSERT(pc_sum == PC_ITEMS * (PC_ITEMS + 1) / 2);
    KASSERT(pc_count[0] > 0 && pc_count[1] > 0);

    /* join:對方睡 100ms,join 應該在它結束的那一刻返回,而不是輪詢的整數倍 */
    uint64_t t0 = timer_ticks();
    unsigned long sw0 = sched_switch_count();
    int tid = thread_create("sleeper", join_sleeper, NULL);
    thread_join(tid);
    kprintf("sync: join returned after %llu ticks, %lu context switches\n",
            (unsigned long long)(timer_ticks() - t0), sched_switch_count() - sw0);
    thread_join(tid);                        /* 已結束的 tid:必須立即返回 */
    thread_join(99999);                      /* 不存在的 tid:同樣立即返回 */
    KASSERT(sem_trywait(&pc_items) == 0);    /* 沒有項目:trywait 不等待,回傳 0 */
}

/*
 * 開機參數 stress:建立大量執行緒,確認數量只受記憶體限制。
 * 每條執行緒讓出 CPU 數次後結束;全部結束後記憶體必須完全歸還。
 */
#define STRESS_THREADS 1000

/* 號誌當作「完成通知」:每條結束前 post 一次,main 就 wait 相同次數 */
static struct semaphore stress_sem = SEMAPHORE_INIT(0);

static void stress_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < 3; i++)
        thread_yield();
    sem_post(&stress_sem);
}

static void sched_stress(void)
{
    size_t free0 = pmm_free_count();
    unsigned long sw0 = sched_switch_count();
    uint64_t t0 = timer_ticks();
    unsigned created = 0;
    struct kheap_stats h0, h1;
    kheap_stats(&h0);

    for (unsigned i = 0; i < STRESS_THREADS; i++) {
        if (thread_create("stress", stress_worker, NULL) < 0)
            break;
        created++;
    }
    kprintf("stress: created %u threads, %zu KB used (stacks + thread structs)\n", created,
            (free0 - pmm_free_count()) * (PAGE_SIZE / 1024));
    for (unsigned i = 0; i < created; i++)
        sem_wait(&stress_sem);               /* 每等到一次代表一條完成 */
    thread_sleep(10);                        /* post 之後它們還要一點時間真正結束並被回收 */
    kprintf("stress: all finished in %llu ticks, %lu context switches\n",
            (unsigned long long)(timer_ticks() - t0), sched_switch_count() - sw0);
    KASSERT(created == STRESS_THREADS && sched_thread_count() == 1);
    /* stack 必須全部歸還。heap 為了放 1000 個 struct thread 而向 pmm 多要的頁
       不會歸還(kmalloc 的已知限制),所以要扣掉;但那些空間在 heap 內必須都已空閒 */
    kheap_stats(&h1);
    size_t heap_grow = h1.heap_bytes - h0.heap_bytes;
    KASSERT(pmm_free_count() == free0 - heap_grow / PAGE_SIZE);
    KASSERT(h1.free_bytes == h0.free_bytes + heap_grow);
    kprintf("stress: ok (stacks reclaimed, heap grew by %zu KB)\n", heap_grow / 1024);
}

/*
 * crash=stack:遞迴到超出 16KB 的 stack,排程器應在下次切換時發現。
 * 每層都把區域陣列整個寫滿:記號只有 stack 最底端的 4 個位元組,
 * 如果只寫陣列的一小部分,可能剛好「跨過」記號而沒有蓋到它,就偵測不到。
 */
static int overflow_recurse(int depth)
{
    volatile char pad[512];
    for (unsigned i = 0; i < sizeof(pad); i++)
        pad[i] = (char)depth;
    if (depth == 0) {
        thread_yield();                      /* 在最深處讓出 -> schedule() 檢查記號 */
        return pad[0];
    }
    return overflow_recurse(depth - 1) + pad[0];
}

static void overflow_worker(void *arg)
{
    (void)arg;
    overflow_recurse(40);
}

/* crash=thread:在執行緒中觸發例外,backtrace 應能走訪該執行緒自己的 stack */
static void fault_worker(void *arg)
{
    (void)arg;
    panic("panic inside thread %d (%s)", thread_id(), thread_name());
}

static void sched_crash_test(const char *cmd)
{
    if (cmdline_has(cmd, "crash=stack")) {
        kprintf("overflowing a thread stack...\n");
        thread_create("overflow", overflow_worker, NULL);
        thread_sleep(100);
    } else if (cmdline_has(cmd, "crash=thread")) {
        thread_create("faulty", fault_worker, NULL);
        thread_sleep(100);
    } else if (cmdline_has(cmd, "crash=relock")) {
        /* 同一條執行緒對同一個 mutex 上鎖兩次:不偵測的話會自己等自己 */
        static struct mutex m = MUTEX_INIT;
        kprintf("locking a mutex twice...\n");
        mutex_lock(&m);
        mutex_lock(&m);
    }
}

/*
 * 記憶體管理的開機自我測試:任何一項不符就 panic。
 * 同時示範 pmm(以頁為單位)與 kmalloc(任意大小)的用法。
 */
static void mem_selftest(void)
{
    size_t free0 = pmm_free_count();

    /* ---- pmm:頁面配置 ---- */
    char *a = pmm_alloc();
    char *b = pmm_alloc();
    KASSERT(a && b);
    KASSERT(b == a + PAGE_SIZE);             /* first-fit:依序拿到相鄰的兩頁 */
    KASSERT((uintptr_t)a % PAGE_SIZE == 0);
    pmm_free(a);
    KASSERT(pmm_alloc() == (void *)a);       /* 釋放後最低的空閒頁會被重複使用 */
    char *c = pmm_alloc_pages(8);            /* 連續 8 頁 */
    KASSERT(c == b + PAGE_SIZE);
    memset(c, 0xA5, 8 * PAGE_SIZE);          /* 整段都要能寫 */
    KASSERT(pmm_free_count() == free0 - 10);
    pmm_free_pages(c, 8);
    pmm_free(b);
    pmm_free(a);
    KASSERT(pmm_free_count() == free0);
    kprintf("pmm: selftest ok (first page %p)\n", (void *)a);

    /* ---- kmalloc:任意大小配置 ---- */
    struct kheap_stats st;
    char *p1 = kmalloc(24);
    char *p2 = kmalloc(100);
    char *p3 = kmalloc(5000);                /* 超過一頁 */
    KASSERT(p1 && p2 && p3);
    KASSERT((uintptr_t)p1 % 16 == 0 && (uintptr_t)p2 % 16 == 0 &&
            (uintptr_t)p3 % 16 == 0);
    memset(p1, 0x11, 24);                    /* 各自寫滿,互相不應覆蓋 */
    memset(p2, 0x22, 100);
    memset(p3, 0x33, 5000);
    KASSERT(p1[23] == 0x11 && p2[0] == 0x22 && p2[99] == 0x22 && p3[0] == 0x33);

    kfree(p2);
    char *p4 = kmalloc(64);                  /* 放得進 p2 留下的洞 -> 同一位址 */
    KASSERT(p4 == p2);
    char *z = kzalloc(64);
    KASSERT(z && z[0] == 0 && z[63] == 0);

    kheap_stats(&st);
    kprintf("kmalloc: p1=%p p2=%p p3=%p, heap %zu bytes, %zu free in %zu block(s)\n",
            (void *)p1, (void *)p2, (void *)p3,
            st.heap_bytes, st.free_bytes, st.free_blocks);

    kfree(p1);
    kfree(p3);
    kfree(p4);
    kfree(z);
    kfree(NULL);                             /* 必須安全地被忽略 */
    kheap_stats(&st);
    /* 全部釋放後,相鄰的空閒區塊應合併回一整塊 */
    KASSERT(st.free_blocks == 1 && st.free_bytes == st.heap_bytes);
    kprintf("kmalloc: selftest ok (all freed, coalesced into 1 block)\n");
}

/*
 * 開機參數 memtest:把所有空閒頁都配置出來、寫入再讀回驗證,最後全部釋放。
 * 用來確認 pmm 認為「可用」的每一頁真的是可讀寫的 RAM,
 * 而且沒有把 kernel、位元圖或 bootloader 的資料配置出去。
 * 配置到的頁用每頁開頭的指標串成串列,就不需要另外的空間來記錄。
 */
static void mem_fulltest(void)
{
    struct page_node {
        struct page_node *next;
        uintptr_t tag;
    } *head = NULL, *n;
    size_t free0 = pmm_free_count(), count = 0;

    while ((n = pmm_alloc()) != NULL) {
        n->next = head;
        n->tag = ~(uintptr_t)n;              /* 每頁寫入與自己位址相關的值 */
        ((char *)n)[PAGE_SIZE - 1] = 0x5A;   /* 頁尾也要能寫 */
        head = n;
        count++;
    }
    KASSERT(count == free0 && pmm_free_count() == 0);
    KASSERT(kmalloc(100000) == NULL);        /* 記憶體用完:回傳 NULL 而不是當掉 */

    while (head) {
        n = head;
        head = n->next;
        KASSERT(n->tag == ~(uintptr_t)n);    /* 沒有兩頁其實是同一塊記憶體 */
        KASSERT(((char *)n)[PAGE_SIZE - 1] == 0x5A);
        pmm_free(n);
    }
    KASSERT(pmm_free_count() == free0);
    kprintf("memtest: %zu pages written and verified\n", count);
}

/* kernel 主函式。主控台此時已可使用。此函式不會返回。 */
void kmain(const struct boot_info *bi)
{
    kprintf("Hello WElyOS\n");
    kprintf("arch: %s (%u-bit)\n", arch_name(), (unsigned)(sizeof(void *) * 8));
    kprintf("cmdline: \"%s\"\n", bi->cmdline);
    kprintf("memory: base=0x%llx size=%llu KB\n",
            (unsigned long long)bi->mem_base,
            (unsigned long long)(bi->mem_size >> 10));
    kprintf("kmain=%p\n", (void *)kmain);

    /* autoexit:自動結束模擬器;並讓 panic 也結束模擬器(回傳失敗碼) */
    int autoexit = cmdline_has(bi->cmdline, "autoexit");
    panic_set_exit(autoexit);

    /* 記憶體管理:先建立頁面配置器,之後才能用 kmalloc */
    pmm_init(bi);
    mem_selftest();
    if (cmdline_has(bi->cmdline, "memtest"))
        mem_fulltest();

    crash_test(bi->cmdline);

    /* 排程器:把目前的流程登記為 main 執行緒,再啟動計時器(100Hz)與中斷。
       從開中斷那一刻起,main 也會和其他執行緒一樣被搶佔 */
    sched_init();
    timer_init(100);
    arch_irq_enable();
    kprintf("timer: %u Hz\n", timer_hz());

    sched_crash_test(bi->cmdline);
    sched_demo();
    sync_demo();
    if (cmdline_has(bi->cmdline, "stress"))
        sched_stress();

    kprintf("uptime: %llu ticks, %lu context switches\n",
            (unsigned long long)timer_ticks(), sched_switch_count());
    if (autoexit)
        arch_exit(0);
    for (;;)
        thread_sleep(1000);                  /* main 無事可做;CPU 交給 idle 執行緒休眠 */
}
