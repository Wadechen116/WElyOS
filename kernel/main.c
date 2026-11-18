/*
 * main.c - kernel 的 C 語言進入點(與架構無關)
 *
 * 由各架構的 arch_start()(arch/<名稱>/start.c)在完成最基本的
 * 硬體初始化、並把開機資訊轉成 struct boot_info 後呼叫。
 */
#include <stdint.h>
#include "arch.h"
#include "blackbox.h"
#include "blk.h"
#include "boot_info.h"
#include "console.h"
#include "cpu.h"
#include "fs.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "kstring.h"
#include "panic.h"
#include "pmm.h"
#include "proc.h"
#include "sched.h"
#include "smp.h"
#include "spinlock.h"
#include "sync.h"
#include "timer.h"
#include "vm.h"
#include "watchdog.h"
#include "mmu.h"
#include "netdev.h"
#include "netif.h"

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
 *   crash=null     讀取 NULL 指標(位址 0 沒有對映 -> page fault)
 *   crash=wtext    寫入 kernel 程式碼(唯讀頁 -> page fault)
 *   crash=exec     跳到資料頁執行(不可執行頁 -> page fault;i386 無此保護)
 *   crash=spin     同一顆 CPU 對同一把 spinlock 上鎖兩次(不偵測的話會永遠卡住)
 *   crash=spinsleep 持有 spinlock 時讓出 CPU(規則禁止;排程器應該擋下)
 *   crash=smp      其他 CPU 都在忙的時候 panic(它們應該被叫停)
 *   crash=wdog     一顆 CPU 關著中斷空轉(卡死):隔壁的 CPU 的看門狗應該發現並 panic
 * (crash=stack、crash=thread、crash=relock、crash=spin* 需要排程器,在 sched_demo 之前另外處理)
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
    } else if (cmdline_has(cmd, "crash=null")) {
        volatile int *p = NULL;
        kprintf("reading from NULL...\n");
        kprintf("read %d?!\n", *p);               /* 不應到達 */
    } else if (cmdline_has(cmd, "crash=wtext")) {
        volatile char *p = (volatile char *)(uintptr_t)kmain;
        kprintf("writing to kernel text at %p...\n", (void *)(uintptr_t)kmain);
        *p = 0;
        kprintf("write succeeded?!\n");           /* 不應到達 */
    } else if (cmdline_has(cmd, "crash=exec")) {
        if (!MMU_HAS_NX) {
            kprintf("crash=exec: this page-table format has no NX bit, skipped\n");
            return;
        }
        void *page = pmm_alloc();                 /* 一般的資料頁:可讀寫、不可執行 */
        memset(page, 0, PAGE_SIZE);
        kprintf("jumping to data page %p...\n", page);
        ((void (*)(void))(uintptr_t)page)();
        kprintf("execution returned?!\n");        /* 不應到達 */
    }
}

/*
 * 分頁示範:查詢權限、把一個實體頁對映到兩個不同的虛擬位址(別名)。
 */
static const char *perm_str(unsigned f, char buf[4])
{
    buf[0] = (f & VM_READ) ? 'r' : '-';
    buf[1] = (f & VM_WRITE) ? 'w' : '-';
    buf[2] = (f & VM_EXEC) ? 'x' : '-';
    buf[3] = '\0';
    return buf;
}

static void vm_demo(void)
{
    static int data_var = 1;
    static const int ro_var = 2;
    uintptr_t pa;
    unsigned f1, f2, f3;
    char b1[4], b2[4], b3[4];

    /* 1. 各區段的權限 */
    KASSERT(vm_query((uintptr_t)kmain, &pa, &f1) == 0 && pa == (uintptr_t)kmain);
    KASSERT(vm_query((uintptr_t)&ro_var, NULL, &f2) == 0);
    KASSERT(vm_query((uintptr_t)&data_var, NULL, &f3) == 0);
    kprintf("vm: kmain %s, rodata %s, data %s\n",
            perm_str(f1, b1), perm_str(f2, b2), perm_str(f3, b3));
    KASSERT(!(f1 & VM_WRITE) && !(f2 & VM_WRITE) && (f3 & VM_WRITE));
    KASSERT(vm_query(0, NULL, NULL) != 0);             /* NULL 頁沒有對映 */

    /* 2. 別名:兩個虛擬位址指向同一個實體頁。這兩個位址都在 RAM 之外,
          原本沒有任何東西;對映之後才變得可以存取 */
    const uintptr_t va1 = 0xC0000000u, va2 = 0xC0001000u;
    unsigned long tables0 = vm_table_pages();
    volatile uint32_t *phys = pmm_alloc();
    KASSERT(phys != NULL);
    KASSERT(vm_query(va1, NULL, NULL) != 0);
    KASSERT(vm_map(va1, (uintptr_t)phys, VM_READ | VM_WRITE) == 0);
    KASSERT(vm_map(va2, (uintptr_t)phys, VM_READ) == 0);

    volatile uint32_t *v1 = (volatile uint32_t *)va1;
    volatile uint32_t *v2 = (volatile uint32_t *)va2;
    *v1 = 0x12345678;                        /* 從第一個虛擬位址寫入 */
    KASSERT(*v2 == 0x12345678);              /* 從第二個虛擬位址讀到 */
    KASSERT(*phys == 0x12345678);            /* 從實體位址(恆等對映)也讀到 */
    *phys = 0xCAFEF00D;
    KASSERT(*v1 == 0xCAFEF00D && *v2 == 0xCAFEF00D);
    KASSERT(vm_query(va2 + 0x10, &pa, &f1) == 0 && pa == (uintptr_t)phys + 0x10);
    kprintf("vm: %p and %p both map to physical %p (%lu new table pages)\n",
            (void *)va1, (void *)va2, (void *)(uintptr_t)phys,
            vm_table_pages() - tables0);

    /* 安全檢查:同時可寫又可執行的對映必須被拒絕 */
    KASSERT(vm_map(va2, (uintptr_t)phys, VM_READ | VM_WRITE | VM_EXEC) != 0);

    KASSERT(vm_unmap(va1) == 0 && vm_unmap(va2) == 0);
    KASSERT(vm_query(va1, NULL, NULL) != 0);
    KASSERT(vm_unmap(va1) != 0);             /* 已移除:再移除要回報失敗 */

    pmm_free((void *)(uintptr_t)phys);

    /* 3. 拆開大分頁:取主記憶體 1/4 處、對齊到大分頁邊界之後的第 2 頁,
          這個位置確定是以大分頁對映的。把它單獨改成唯讀,vm 必須自動把
          那一大塊拆成 4KB 頁(多用 1 頁表格),而且只影響這一頁,鄰居維持可寫 */
    uintptr_t large = (uintptr_t)PAGE_SIZE << (MMU_LARGE_LEVEL * MMU_BITS);
    uintptr_t tgt = ((pmm_mem_start() + pmm_total_pages() / 4 * PAGE_SIZE) & ~(large - 1))
                    + PAGE_SIZE;
    volatile uint32_t *tp = (volatile uint32_t *)tgt;
    volatile uint32_t *nb = (volatile uint32_t *)(tgt + PAGE_SIZE);
    uint32_t tp_old = *tp, nb_old = *nb;
    *tp = 0x600DF00D;
    tables0 = vm_table_pages();
    KASSERT(vm_map(tgt, tgt, VM_READ) == 0);
    KASSERT(vm_table_pages() - tables0 == 1);            /* 拆分用掉剛好 1 頁表格 */
    KASSERT(vm_query(tgt + 4, &pa, &f1) == 0 && pa == tgt + 4 && !(f1 & VM_WRITE));
    KASSERT(vm_query(tgt + PAGE_SIZE, &pa, &f2) == 0 && pa == tgt + PAGE_SIZE);
    KASSERT(f2 & VM_WRITE);
    KASSERT(*tp == 0x600DF00D);              /* 內容不受拆分影響 */
    *nb = nb_old;                            /* 鄰居仍可寫入 */
    kprintf("vm: split a %lu MB page to write-protect %p (1 new table page)\n",
            (unsigned long)(large >> 20), (void *)tgt);
    KASSERT(vm_map(tgt, tgt, VM_READ | VM_WRITE) == 0);
    *tp = tp_old;
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

/* 每條執行緒記錄兩個數字:
     iters  迴圈跑了幾圈(看得出它有在執行)
     ticks  排程器記在它頭上的 CPU 時間(tick 數;和時間片用的是同一份記帳)
   公平性看的是 ticks。iters 不能拿來比:同樣的時間能跑幾圈,在模擬器上會受
   「這條執行緒的 stack 剛好配置在哪一頁」影響到好幾倍(見 DEVLOG 第 21 步) */
struct spin_stat {
    volatile unsigned long iters;
    volatile unsigned long ticks;
};

static void spin_worker(void *arg)
{
    struct spin_stat *st = arg;
    unsigned long t0 = thread_cpu_ticks();

    while (timer_ticks() < spin_deadline)
        st->iters++;
    st->ticks = thread_cpu_ticks() - t0;
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
    unsigned threads0 = sched_thread_count();    /* 開始時已經存在的執行緒(main 與常駐的 kernel 執行緒) */
    sched_trim();                            /* 基準:快取中不留閒置的 stack */
    size_t free0 = pmm_free_count() + vm_table_pages();
    struct kheap_stats h0, h1;
    int t[3];
    kheap_stats(&h0);
    size_t pool0 = sched_pool_bytes();       /* 執行緒結構留在池子裡不歸還,核對時要扣掉 */

    kprintf("sched: cooperative (yield)\n");
    t[0] = thread_create("A", coop_worker, NULL);
    t[1] = thread_create("B", coop_worker, NULL);
    t[2] = thread_create("C", coop_worker, NULL);
    join_all(t, 3);

    kprintf("sched: preemptive (busy loops, no yield)\n");
    static struct spin_stat s1, s2;
    unsigned long sw0 = sched_switch_count();
    spin_deadline = timer_ticks() + timer_hz() * 60 / 1000;   /* 各自空轉 60ms = 6 個時間片 */
    t[0] = thread_create("spin1", spin_worker, &s1);
    t[1] = thread_create("spin2", spin_worker, &s2);
    join_all(t, 2);
    kprintf("  spin1: %lu ms of cpu (%lu loops), spin2: %lu ms (%lu loops), %lu context switches\n",
            s1.ticks, s1.iters, s2.ticks, s2.iters, sched_switch_count() - sw0);
    /* 兩條都要跑到,而且得到的 CPU 時間相近(差距在 2 倍以內) */
    unsigned long c1 = s1.ticks, c2 = s2.ticks;
    KASSERT(s1.iters > 0 && s2.iters > 0 && c1 > 0 && c2 > 0 && c1 < c2 * 2 && c2 < c1 * 2);

    kprintf("sched: sleep\n");
    t[0] = thread_create("s30", sleep_worker, (void *)30);
    t[1] = thread_create("s10", sleep_worker, (void *)10);
    t[2] = thread_create("s20", sleep_worker, (void *)20);
    join_all(t, 3);

    /* 所有示範執行緒都結束後,它們的 stack 與結構都應該已歸還 */
    sched_trim();                            /* 快取中閒置的 stack 也要還回去才能核對 */
    kheap_stats(&h1);
    KASSERT(sched_thread_count() == threads0);
    /* 保護頁會把所在的大分頁拆開,多出來的頁表不會歸還,所以一併計入。
       heap 也一樣:執行緒結構的池子成長時,heap 可能向 pmm 多要了分頁,那些不會歸還 */
    size_t heap_grown = h1.heap_bytes - h0.heap_bytes;
    KASSERT(pmm_free_count() + vm_table_pages() + heap_grown / PAGE_SIZE == free0);
    KASSERT(h1.free_bytes + (sched_pool_bytes() - pool0) == h0.free_bytes + heap_grown);
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
    thread_sleep(20);
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

    /* join:對方睡 20ms,join 應該在它結束的那一刻返回,而不是輪詢的整數倍 */
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

/* ------------------------------------------------------------------ */
/* 優先權示範                                                          */
/* ------------------------------------------------------------------ */

/* 佔用 CPU 直到經過 ms 毫秒(以系統時鐘計;期間被搶佔的時間也算在內) */
static void busy_ms(unsigned ms)
{
    uint64_t end = timer_ticks() + ms * timer_hz() / 1000;   /* 32 位元乘除:避開 i386 的 __udivdi3 */
    while (timer_ticks() < end)
        ;
}

/* 示範 1:執行順序。每條執行緒開始執行時把自己的代號記下來 */
static char prio_log[4];
static volatile int prio_log_n;

static void prio_worker(void *arg)
{
    prio_log[prio_log_n++] = (char)(uintptr_t)arg;
    busy_ms(3);
}

/* 示範 2:搶佔延遲。高優先權的執行緒睡 5ms,醒來時 CPU 正被別人佔用 */
static volatile unsigned latency_ticks;

static void latency_sleeper(void *arg)
{
    (void)arg;
    uint64_t t0 = timer_ticks();
    thread_sleep(5);
    latency_ticks = (unsigned)(timer_ticks() - t0);
}

static void latency_spinner(void *arg)
{
    (void)arg;
    /* 必須比「對方開始睡之前的等待(至多一個時間片)+ 睡眠 + 再一個時間片」還長,
       對方醒來時這裡才確定還在佔用 CPU;太短的話兩組對照會得到一樣的結果 */
    busy_ms(30);
}

static unsigned latency_run(int sleeper_prio)
{
    int t[2];
    t[0] = thread_create_prio("spinner", latency_spinner, NULL, PRIO_NORMAL);
    t[1] = thread_create_prio("sleeper", latency_sleeper, NULL, sleeper_prio);
    join_all(t, 2);
    return latency_ticks;
}

/*
 * 示範 3:優先權反轉與優先權繼承。三條執行緒:
 *   low    先拿到鎖,需要再工作 6ms 才會解鎖
 *   medium 純粹佔用 CPU 20ms,和這把鎖無關
 *   high   要拿同一把鎖
 * 沒有繼承:medium 比 low 優先,low 輪不到、無法解鎖,high 得等 medium 跑完。
 * 有繼承:high 等鎖時把 low 提高到和自己一樣,low 搶過 medium、很快解鎖。
 */
static struct mutex inv_mutex;
static volatile int inv_locked;
static volatile unsigned inv_wait;

static void inv_low(void *arg)
{
    (void)arg;
    mutex_lock(&inv_mutex);
    inv_locked = 1;
    busy_ms(6);
    mutex_unlock(&inv_mutex);
}

static void inv_medium(void *arg)
{
    (void)arg;
    busy_ms(20);
}

static void inv_high(void *arg)
{
    (void)arg;
    uint64_t t0 = timer_ticks();
    mutex_lock(&inv_mutex);
    inv_wait = (unsigned)(timer_ticks() - t0);
    mutex_unlock(&inv_mutex);
}

static unsigned inversion_run(int inherit)
{
    int t[3];

    mutex_init(&inv_mutex);
    inv_mutex.inherit = inherit;
    inv_locked = 0;

    t[0] = thread_create_prio("low", inv_low, NULL, PRIO_LOW);
    thread_sleep(2);                         /* main 睡著,low 才有機會先拿到鎖 */
    KASSERT(inv_locked);
    t[1] = thread_create_prio("medium", inv_medium, NULL, PRIO_NORMAL);
    t[2] = thread_create_prio("high", inv_high, NULL, PRIO_HIGH);
    join_all(t, 3);
    return inv_wait;
}

/*
 * 示範 4:完整的優先權繼承。
 *   連鎖:L 持有鎖 A;M 持有鎖 B 並等待 A;H 等待 B。
 *         H 的優先權要經過 M 傳到 L,L 才能搶過其他中等優先權的執行緒。
 *   多把鎖:L 同時持有 A 和 C,H 在等 A。L 先解開 C 時不該失去提升。
 */
static struct mutex pi_a, pi_b, pi_c;
static volatile int pi_stage;
static volatile int pi_l_prio_chained, pi_l_prio_after_c, pi_l_prio_after_a;

static void pi_low(void *arg)
{
    (void)arg;
    mutex_lock(&pi_a);
    mutex_lock(&pi_c);
    pi_stage = 1;                            /* 兩把鎖都到手 */
    while (pi_stage < 3)                     /* 等 M、H 都排上來 */
        thread_yield();
    pi_l_prio_chained = thread_priority();   /* 應該已經是 H 的優先權 */
    mutex_unlock(&pi_c);                     /* 解開沒人等的 C */
    pi_l_prio_after_c = thread_priority();   /* A 還有人等:提升必須保留 */
    mutex_unlock(&pi_a);
    pi_l_prio_after_a = thread_priority();   /* 全部解開:回到自己的優先權 */
}

static void pi_mid(void *arg)
{
    (void)arg;
    mutex_lock(&pi_b);
    pi_stage = 2;
    mutex_lock(&pi_a);                       /* 被 L 擋住 */
    mutex_unlock(&pi_a);
    mutex_unlock(&pi_b);
}

static void pi_high(void *arg)
{
    (void)arg;
    pi_stage = 3;
    mutex_lock(&pi_b);                       /* 被 M 擋住,而 M 被 L 擋住 */
    mutex_unlock(&pi_b);
}

/* 示範 5:防飢餓。一條高優先權的執行緒完全不睡,低優先權的仍應得到一些 CPU */
static volatile unsigned long starve_count;
static volatile int starve_stop;

static void starve_hog(void *arg)
{
    (void)arg;
    busy_ms(150);
    starve_stop = 1;
}

static void starve_victim(void *arg)
{
    (void)arg;
    while (!starve_stop)
        starve_count++;
}

static void prio_demo(void)
{
    int t[3];

    /* main 暫時升到最高:建立的三條在 main 開始等待之前都不會執行 */
    thread_set_priority(PRIO_MAX);

    kprintf("prio: three threads created as low, normal, high\n");
    prio_log_n = 0;
    t[0] = thread_create_prio("low", prio_worker, (void *)'L', PRIO_LOW);
    t[1] = thread_create_prio("normal", prio_worker, (void *)'N', PRIO_NORMAL);
    t[2] = thread_create_prio("high", prio_worker, (void *)'H', PRIO_HIGH);
    KASSERT(prio_log_n == 0);                /* main 最優先,還沒有人跑過 */
    join_all(t, 3);
    prio_log[3] = '\0';
    kprintf("  run order: %s (expected HNL)\n", prio_log);
    KASSERT(prio_log[0] == 'H' && prio_log[1] == 'N' && prio_log[2] == 'L');

    unsigned same = latency_run(PRIO_NORMAL);
    unsigned high = latency_run(PRIO_HIGH);
    kprintf("prio: sleep(5 ms) while another thread spins: same priority woke after "
            "%u ticks, high priority after %u\n", same, high);
    /* 高優先權:約 6 個 tick(睡眠本身)就醒來;同優先權要多等對方的時間片。
       不檢查絕對數字:模擬器上 tick 可能一次補記好幾個,會有 1~3 個 tick 的抖動 */
    KASSERT(high < same);

    unsigned without = inversion_run(0);
    unsigned with = inversion_run(1);
    kprintf("prio: inversion: high waited %u ticks for the lock without inheritance, "
            "%u with\n", without, with);
    KASSERT(with < without && with <= 10 && without >= 15);

    /* 完整的繼承。此時 main 是最高優先權,各執行緒靠 main 睡覺的空檔依序就位 */
    mutex_init(&pi_a);
    mutex_init(&pi_b);
    mutex_init(&pi_c);
    pi_stage = 0;
    t[0] = thread_create_prio("pi-low", pi_low, NULL, PRIO_LOW);
    while (pi_stage < 1)
        thread_sleep(1);
    t[1] = thread_create_prio("pi-mid", pi_mid, NULL, PRIO_NORMAL);
    while (pi_stage < 2)
        thread_sleep(1);
    thread_sleep(1);                         /* 讓 M 走到等待 A 的地方 */
    t[2] = thread_create_prio("pi-high", pi_high, NULL, PRIO_HIGH);
    join_all(t, 3);
    kprintf("prio: chained inheritance: low thread ran at priority %d, %d after "
            "releasing an unrelated lock, %d after releasing all\n",
            pi_l_prio_chained, pi_l_prio_after_c, pi_l_prio_after_a);
    KASSERT(pi_l_prio_chained == PRIO_HIGH);     /* H -> M -> L 傳遞成功 */
    KASSERT(pi_l_prio_after_c == PRIO_HIGH);     /* 解開無關的鎖不失去提升 */
    KASSERT(pi_l_prio_after_a == PRIO_LOW);      /* 全部解開後恢復 */

    /* 防飢餓:hog(高優先權)連續佔用 CPU 150ms,victim(低優先權)只能靠提升執行 */
    starve_count = 0;
    starve_stop = 0;
    unsigned long boosts0 = sched_starve_boosts();
    t[0] = thread_create_prio("victim", starve_victim, NULL, PRIO_LOW);
    t[1] = thread_create_prio("hog", starve_hog, NULL, PRIO_HIGH);
    thread_set_priority(PRIO_NORMAL);        /* main 不參與;降回一般優先權去等待 */
    thread_join(t[1]);
    unsigned long during = starve_count;     /* hog 還在跑的期間 victim 做了多少 */
    thread_join(t[0]);
    kprintf("prio: starvation guard: low thread got %lu boosts and made progress "
            "(%lu iterations) under a 150 ms high-priority hog\n",
            sched_starve_boosts() - boosts0, during);
    KASSERT(during > 0 && sched_starve_boosts() - boosts0 >= 1);

    thread_set_priority(PRIO_NORMAL);
}

/* ------------------------------------------------------------------ */
/* 使用者模式示範                                                      */
/* ------------------------------------------------------------------ */

/* cmdline 裡有沒有出現 needle 這段文字(不要求是完整的單字) */
static int strstr_simple(const char *hay, const char *needle)
{
    size_t n = strlen(needle);

    for (; *hay; hay++)
        if (strncmp(hay, needle, n) == 0)
            return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 網路卡                                                              */
/* ------------------------------------------------------------------ */

static struct netdev *boot_nic;

/*
 * 網路卡驅動程式的測試(不經過協定堆疊):手工組一個 ARP 詢問「10.0.2.2 的硬體位址是什麼」,
 * 送出去,然後等回答。模擬器的使用者模式網路會以閘道的身分回答,
 * 所以收得到就表示傳送與接收兩個方向都通。
 */
static void nic_selftest(void)
{
    static uint8_t frame[1600];
    static const uint8_t my_ip[4] = { 10, 0, 2, 15 }, gw_ip[4] = { 10, 0, 2, 2 };
    struct netdev *d = boot_nic;

    if (!d) {
        kprintf("net: no network card, tests skipped\n");
        return;
    }
    memset(frame, 0, 42);
    memset(frame, 0xFF, 6);                  /* 目的:廣播 */
    memcpy(frame + 6, d->mac, 6);
    frame[12] = 0x08; frame[13] = 0x06;      /* 類型:ARP */
    frame[14] = 0; frame[15] = 1;            /* 硬體:乙太網路 */
    frame[16] = 0x08; frame[17] = 0;         /* 協定:IPv4 */
    frame[18] = 6; frame[19] = 4;            /* 位址長度 */
    frame[20] = 0; frame[21] = 1;            /* 詢問 */
    memcpy(frame + 22, d->mac, 6);
    memcpy(frame + 28, my_ip, 4);
    memcpy(frame + 38, gw_ip, 4);
    KASSERT(d->send(d, frame, 42) == 0);
    KASSERT(d->send(d, frame, 10) == -E_INVAL && d->send(d, frame, 1515) == -E_INVAL);   /* 長度不合理 */

    uint64_t t0 = timer_ticks();
    int got = 0;
    while (!got && timer_ticks() - t0 < 500) {
        int n = d->recv(d, frame, sizeof(frame));
        KASSERT(n >= 0 && n <= 1514);
        /* 是 ARP 的回答、問的是閘道、而且是回給我們的 */
        if (n >= 42 && frame[12] == 0x08 && frame[13] == 0x06 && frame[21] == 2 &&
            !memcmp(frame + 28, gw_ip, 4) && !memcmp(frame, d->mac, 6))
            got = 1;
        else if (n == 0)
            thread_sleep(1);
    }
    KASSERT(got);
    kprintf("net: ok (%s: sent an ARP request, the gateway 10.0.2.2 answered from "
            "%02x:%02x:%02x:%02x:%02x:%02x after %u ms)\n", d->name, frame[22], frame[23], frame[24],
            frame[25], frame[26], frame[27], (unsigned)(timer_ticks() - t0));
}

/* ------------------------------------------------------------------ */
/* 磁碟                                                                */
/* ------------------------------------------------------------------ */

/*
 * 巡檢執行緒:每 500ms 核對 16 個區塊的檢查碼,一遍又一遍地走過整顆磁碟
 * (16MB 的磁碟約兩分鐘一輪)。刻意做得很慢:它的工作不急,不該和正常的讀寫搶磁碟。
 * 發現損壞時記進黑盒子(每一輪最多記一次,免得同一個壞區塊灌滿紀錄)。
 */
static int scrub_running;

static void scrub_thread(void *arg)
{
    uint32_t pos = 1;
    unsigned bad_this_pass = 0;

    (void)arg;
    for (;;) {
        thread_sleep(500);
        uint32_t before = pos;
        unsigned bad = wfs_scrub(&pos, 16);
        if (bad && !bad_this_pass)
            blackbox_log(BB_DISK, "scrub: %u corrupted block(s) found near block %u", bad, before);
        bad_this_pass += bad;
        if (pos < before)                    /* 繞回開頭:新的一輪 */
            bad_this_pass = 0;
    }
}

/* 常駐的 kernel 執行緒數(main 以外):網路的接收執行緒、磁碟的巡檢執行緒。測試核對執行緒數時使用 */
static unsigned resident_threads(void)
{
    return (boot_nic != NULL) + (unsigned)scrub_running;
}

/* 開機時找到的磁碟(包了一層可以故意出錯的那一個);沒有磁碟為 NULL */
static struct blkdev *boot_disk;
static struct blkdev *arch_blk_probe_cached(void) { return boot_disk; }

/* 找出磁碟,把上面的檔案系統掛載到 /disk。沒有磁碟、或磁碟上不是我們的檔案系統時,
   只印出訊息,系統照常使用記憶體檔案系統 */
/*
 * ---- 安全模式 ----
 *
 * 開機本身也可能失敗:掛載磁碟時當掉、某個驅動程式在這台機器上出錯…。如果每次開機都走同一條路,
 * 系統就會永遠卡在「開機、當掉、開機、當掉」,而且因為根本到不了可以操作的地方,沒有人能介入。
 *
 * 做法:黑盒子裡每次開機有一筆紀錄,順利完成時再記一筆(BB_HEALTHY)。
 * 開機時數一數「最近連續幾次沒有順利完成」;到達 SAFE_AFTER 次就改以安全模式開機 —
 * 只啟動最少的東西:不掛載磁碟上的檔案系統、不啟動網路、其他 CPU 不參與排程、不跑背景工作。
 * 目的只有一個:讓人進得來,用 faultlog 看看發生了什麼事。
 * 在安全模式裡正常離開(exit)會記下「順利」,下一次開機再試一次完整的開機。
 *
 * 開機參數:safe = 強制以安全模式開機;nosafe = 不要自動進入(除錯用)。
 */
#define SAFE_AFTER 3
static int safe_mode;

static void disk_init(const char *cmdline)
{
    /* 外面包一層可以故意出錯的裝置(測試磁碟錯誤時使用;平常只是原樣轉送) */
    struct blkdev *dev = blk_fault_wrap(arch_blk_probe());
    struct inode *fsroot;
    struct wfs_stats st;
    struct bb_record last;
    unsigned failed = 0;
    /* 故意當機的測試(crash=、disktest=)不該被算成開機失敗;crash=boot 例外,它就是用來測這個的 */
    int test_boot = (strstr_simple(cmdline, "crash=") || strstr_simple(cmdline, "disktest=")) &&
                    !cmdline_has(cmdline, "crash=boot");

    boot_disk = dev;
    safe_mode = cmdline_has(cmdline, "safe");
    if (!dev) {
        kprintf("disk: none\n");
        if (safe_mode)
            kprintf("\n*** SAFE MODE (requested on the command line) ***\n\n");
        return;
    }

    /* 黑盒子:要在掛載之前啟用,才能在碰磁碟上的檔案系統之前決定要不要進安全模式 */
    blackbox_init(dev, wfs_probe(dev));
    if (!blackbox_enabled()) {
        kprintf("blackbox: disabled (no room on this disk, or its contents are not ours)\n");
    } else {
        failed = blackbox_failed_boots();
        if (blackbox_read(0, &last) != 0)
            kprintf("blackbox: empty (first boot with this disk)\n");
        else if (last.type == BB_SHUTDOWN || last.type == BB_HEALTHY)
            kprintf("blackbox: %u records; the previous boot %s\n", blackbox_count(),
                    last.type == BB_SHUTDOWN ? "shut down cleanly" : "came up but was not shut down");
        else if (last.type == BB_PANIC || last.type == BB_WATCHDOG)
            kprintf("blackbox: %u records; the previous boot ended with: %s\n", blackbox_count(), last.text);
        else
            kprintf("blackbox: %u records; the previous boot did not shut down (power loss, or the "
                    "emulator was closed)\n", blackbox_count());
        if (failed >= SAFE_AFTER && !cmdline_has(cmdline, "nosafe"))
            safe_mode = 1;
        blackbox_log(BB_BOOT, "boot #%u on %s%s%s", blackbox_boot(), arch_name(),
                     safe_mode ? ", safe mode" : "", test_boot ? " (test)" : "");
    }
    if (safe_mode) {
        kprintf("\n*** SAFE MODE ***\n");
        if (failed >= SAFE_AFTER)
            kprintf("The last %u boots did not complete.\n", failed);
        kprintf("The disk is not mounted, the network is off and only one CPU is used.\n"
                "Use \"faultlog\" to see what happened; \"exit\" to try a normal boot next time.\n\n");
        return;
    }

    int rc = wfs_mount(dev, &fsroot);
    if (rc < 0) {
        kprintf("disk: %s, %llu MB: not mounted (error %d)%s\n", dev->name,
                (unsigned long long)(dev->sectors / 2048), rc,
                rc == -E_INVAL ? "; it does not hold a WFS file system and was left untouched" :
                rc == -E_NOSPC ? "; it is too small" : "");
        return;
    }
    if (vfs_mkdir("/disk") != 0 || vfs_mount("/disk", fsroot) != 0)
        panic("disk: cannot mount on /disk");
    wfs_stats(&st);
    kprintf("disk: %s, %llu MB, %s on /disk (mount #%u, %u KB free)\n", dev->name,
            (unsigned long long)(dev->sectors / 2048),
            rc == 1 ? "new file system created and mounted" : "mounted",
            st.mounts, st.free_blocks * (unsigned)(PAGE_SIZE / 1024));
    if (st.sb_restored)
        kprintf("disk: superblock restored from its backup copy\n");
    if (st.checksums && thread_create_prio("scrub", scrub_thread, NULL, PRIO_LOW) > 0)
        scrub_running = 1;                   /* 低優先權:只在沒有別的事要做時才巡檢 */
    if (st.repairs)
        blackbox_log(BB_NOTE, "file system check repaired %u problem(s) at mount", st.repairs);
    if (st.sb_restored)
        blackbox_log(BB_DISK, "primary superblock was damaged; restored from the backup copy");
}

/* 關機(結束模擬器)。先把磁碟上的檔案系統正常卸載 */
static void shutdown(int code)
{
    struct wfs_stats st;

    wfs_stats(&st);
    if (st.failed)
        blackbox_log(BB_DISK, "shutting down with the file system read-only after a write error");
    if (safe_mode)
        blackbox_log(BB_HEALTHY, "safe mode session ended normally; the next boot will be a normal one");
    blackbox_log(BB_SHUTDOWN, "clean shutdown, exit code %d", code);
    wfs_shutdown();
    arch_exit(code);
}

/* 測試資料:第 i 個位元組的內容(和位置有關,讀回來時可以核對) */
static char disk_pattern(size_t i, unsigned seed)
{
    return (char)('A' + (i * 7 + i / 4096 + seed) % 53);
}

static void disk_write_file(const char *path, size_t size, unsigned seed)
{
    static char buf[3000];
    struct file *f;

    KASSERT(vfs_open(path, O_CREAT | O_WRONLY | O_TRUNC, &f) == 0);
    for (size_t off = 0; off < size; ) {
        size_t n = size - off < sizeof(buf) ? size - off : sizeof(buf);
        for (size_t i = 0; i < n; i++)
            buf[i] = disk_pattern(off + i, seed);
        KASSERT(vfs_write(f, buf, n) == (long)n);
        off += n;
    }
    vfs_close(f);                            /* 關閉時寫回磁碟 */
}

static void disk_verify_file(const char *path, size_t size, unsigned seed)
{
    static char buf[3000];
    struct file *f;
    size_t off = 0;
    long n;

    KASSERT(vfs_open(path, O_RDONLY, &f) == 0);
    KASSERT(vfs_size(f) == size);
    while ((n = vfs_read(f, buf, sizeof(buf))) > 0) {
        for (long i = 0; i < n; i++)
            KASSERT(buf[i] == disk_pattern(off + (size_t)i, seed));
        off += (size_t)n;
    }
    KASSERT(n == 0 && off == size);
    vfs_close(f);
}

/* 多條執行緒同時在磁碟上建立、寫入、讀回、刪除各自的檔案 */
#define DISK_WORKERS 4
#define DISK_FILES   6

static void disk_worker(void *arg)
{
    int id = (int)(uintptr_t)arg;
    char path[32] = "/disk/selftest/w0-0", data[600], back[600];
    struct file *f;

    for (unsigned i = 0; i < sizeof(data); i++)
        data[i] = (char)(id * 37 + i);
    for (int n = 0; n < DISK_FILES; n++) {
        path[16] = (char)('0' + id);
        path[18] = (char)('0' + n);
        KASSERT(vfs_open(path, O_CREAT | O_RDWR, &f) == 0);
        KASSERT(vfs_write(f, data, sizeof(data)) == (long)sizeof(data));
        KASSERT(vfs_pread(f, back, sizeof(back), 0) == (long)sizeof(back));
        KASSERT(memcmp(data, back, sizeof(data)) == 0);
        vfs_close(f);
        KASSERT(vfs_unlink(path) == 0);
    }
}

/* 磁碟上的檔案系統的自我測試。所有東西都放在 /disk/selftest 底下,結束時刪除,
   所以不影響磁碟上原有的內容,空閒的空間也要回到測試之前的數量 */
static void disk_demo(void)
{
    struct wfs_stats s0, s1, s2;
    struct file *f, *g;
    struct stat st;
    struct dirent e;
    char buf[16];

    wfs_stats(&s0);
    if (!s0.mounted) {
        kprintf("disk: no file system mounted, tests skipped\n");
        return;
    }
    /* 上一次測試如果中途被打斷,可能留下東西:先清掉。順便讓根目錄至少有一個區塊,
       之後的空間核對才有固定的基準 */
    vfs_unlink("/disk/selftest/big");
    vfs_unlink("/disk/selftest/prog");
    vfs_unlink("/disk/selftest");
    KASSERT(vfs_mkdir("/disk/selftest") == 0 && vfs_unlink("/disk/selftest") == 0);
    wfs_stats(&s0);

    /* 目錄與路徑:掛載點兩邊都走得通 */
    KASSERT(vfs_mkdir("/disk/selftest") == 0);
    KASSERT(vfs_mkdir("/disk/selftest") == -E_EXIST);
    KASSERT(vfs_stat("/disk/selftest/..", &st) == 0 && st.type == I_DIR);
    KASSERT(vfs_stat("/disk/../etc/motd", &st) == 0 && st.type == I_FILE);   /* 從磁碟往上回到記憶體檔案系統 */
    KASSERT(vfs_stat("/disk/selftest/../../disk/selftest", &st) == 0);
    KASSERT(vfs_unlink("/disk") == -E_BUSY);                                 /* 掛載點不可以刪除 */
    KASSERT(vfs_mkdev("/disk/selftest/dev", console_dev_ops()) == -E_INVAL); /* 裝置節點只能在記憶體裡 */
    wfs_stats(&s1);
    KASSERT(s1.free_inodes == s0.free_inodes - 1);

    /* 一個 100000 位元組的檔案:25 個資料區塊,超過 10 個之後要用到間接區塊 */
    disk_write_file("/disk/selftest/big", 100000, 1);
    disk_verify_file("/disk/selftest/big", 100000, 1);
    wfs_stats(&s2);
    /* 25 個資料 + 1 個間接 + 1 個給 selftest 目錄放紀錄(目錄也是存在區塊裡的) */
    KASSERT(s2.free_blocks == s1.free_blocks - 27);
    KASSERT(s2.free_inodes == s1.free_inodes - 1);
    KASSERT(vfs_stat("/disk/selftest/big", &st) == 0 && st.size == 100000 && st.nlink == 1);
    KASSERT(vfs_unlink("/disk/selftest") == -E_NOTEMPTY);

    /* 列出目錄:只有這一個檔案 */
    KASSERT(vfs_open("/disk/selftest", O_RDONLY, &f) == 0);
    KASSERT(vfs_readdir(f, &e) == 0 && !strcmp(e.name, "big") && e.type == I_FILE);
    KASSERT(vfs_readdir(f, &e) == 1);
    vfs_close(f);

    /* 換成比較短的內容:多出來的區塊要歸還 */
    disk_write_file("/disk/selftest/big", 5000, 2);
    disk_verify_file("/disk/selftest/big", 5000, 2);
    wfs_stats(&s2);
    KASSERT(s2.free_blocks == s1.free_blocks - 3);       /* 2 個資料 + 目錄的 1 個 */

    /* 附加:接在最後 */
    KASSERT(vfs_open("/disk/selftest/big", O_WRONLY | O_APPEND, &f) == 0);
    KASSERT(vfs_write(f, "tail", 4) == 4);
    vfs_close(f);
    KASSERT(vfs_open("/disk/selftest/big", O_RDONLY, &f) == 0);
    KASSERT(vfs_size(f) == 5004 && vfs_pread(f, buf, 8, 5000) == 4 && !memcmp(buf, "tail", 4));

    /* 開著的時候刪除:名稱立刻消失,內容留到關閉為止(和記憶體檔案系統相同的規則) */
    KASSERT(vfs_unlink("/disk/selftest/big") == 0);
    KASSERT(vfs_stat("/disk/selftest/big", &st) == -E_NOENT);
    KASSERT(vfs_open("/disk/selftest/big", O_RDONLY, &g) == -E_NOENT);
    KASSERT(vfs_pread(f, buf, 4, 5000) == 4 && !memcmp(buf, "tail", 4));
    wfs_stats(&s2);
    KASSERT(s2.free_blocks == s1.free_blocks - 3);       /* 還沒釋放 */
    vfs_close(f);
    wfs_stats(&s2);
    KASSERT(s2.free_blocks == s1.free_blocks - 1 && s2.free_inodes == s1.free_inodes);

    /* 多條執行緒同時使用:各自建立、寫入、讀回、刪除自己的檔案 */
    int tid[DISK_WORKERS];
    for (int i = 0; i < DISK_WORKERS; i++)
        tid[i] = thread_create("disk-worker", disk_worker, (void *)(uintptr_t)i);
    for (int i = 0; i < DISK_WORKERS; i++)
        thread_join(tid[i]);
    wfs_stats(&s2);
    KASSERT(s2.free_blocks == s1.free_blocks - 1 && s2.free_inodes == s1.free_inodes);

    /* 檢查碼:把一個檔案的資料在磁碟上翻轉一個位元(模擬儲存媒體的位元錯誤)。
       讀取必須失敗,而不是把壞掉的內容交出來;巡檢也要找得到它 */
    wfs_stats(&s2);
    if (s2.checksums) {
        static char cbuf[3000];
        uint32_t pos = 1;
        unsigned bad = 0;
        long n;

        disk_write_file("/disk/selftest/flip", 9000, 5);
        KASSERT(wfs_test_drop_cache() == 0);
        KASSERT(wfs_scrub(&pos, wfs_block_count()) == 0);            /* 目前整顆磁碟都是好的 */
        KASSERT(vfs_open("/disk/selftest/flip", O_RDONLY, &f) == 0);
        uint32_t blk = wfs_test_file_block(vfs_file_inode(f), 1);    /* 檔案的第 2 個區塊 */
        KASSERT(blk != 0);
        KASSERT(blk_fault_flip((uint64_t)blk * 8 + 3, 100, 5) == 0);
        while ((n = vfs_read(f, cbuf, sizeof(cbuf))) > 0)
            ;
        KASSERT(n == -E_IO);                                         /* 讀到壞掉的那一塊:拒絕 */
        vfs_close(f);
        wfs_stats(&s2);
        KASSERT(s2.csum_errors >= 1);
        pos = 1;
        for (uint32_t done = 0; done < wfs_block_count(); done += 256)
            bad += wfs_scrub(&pos, 256);
        KASSERT(bad == 1);                                           /* 巡檢找到的正好是那一塊 */
        /* 檔案系統本身沒有壞:刪掉那個檔案(壞掉的區塊被釋放)之後,巡檢又是乾淨的 */
        KASSERT(vfs_unlink("/disk/selftest/flip") == 0);
        pos = 1;
        KASSERT(wfs_scrub(&pos, wfs_block_count()) == 0);
        kprintf("disk: a single flipped bit on the disk was detected: the read failed with an I/O "
                "error and the scrub located 1 bad block among %lu checked\n",
                (unsigned long)(s2.scrub_blocks ? s2.scrub_blocks : 1));
    }

    /* 全部刪除後,空間回到測試之前;再做一次完整檢查,應該找不到任何問題 */
    KASSERT(vfs_unlink("/disk/selftest") == 0);
    KASSERT(vfs_sync() == 0);
    wfs_stats(&s2);
    KASSERT(s2.free_blocks == s0.free_blocks && s2.free_inodes == s0.free_inodes);
    KASSERT(wfs_check_now() == 0);
    kprintf("disk: ok (100000-byte file through an indirect block, truncate, append, unlink while "
            "open, %d threads x %d files; %lu block reads, %lu block writes; "
            "consistency check found nothing)\n", DISK_WORKERS, DISK_FILES,
            s2.disk_reads - s0.disk_reads, s2.disk_writes - s0.disk_writes);
}

/*
 * 跨開機的測試(回歸測試依序以 disktest=1 ~ 5 開機五次,用同一個磁碟映像):
 *   1  寫入一個檔案,正常關機
 *   2  確認檔案還在、內容正確(資料真的留在磁碟上);然後故意把檔案系統弄壞,不正常關機
 *   3  確認掛載時發現並修復了問題,原本的檔案完好
 *   4  磁碟錯誤:讀取失敗只影響那一次操作;寫入失敗之後檔案系統轉為唯讀
 *   5  確認上一次的唯讀狀態讓這次掛載做了檢查,檔案完好,又可以寫入;刪除測試檔
 *   6  黑盒子:留下一筆註記,然後故意 panic(這一階段預期以失敗的結束碼結束)
 *   7  確認黑盒子裡有上一次的註記與 panic 的訊息;把最新的一筆弄壞,確認它被認出來並略過
 *   8  寫一個檔案、正常卸載,然後把磁碟上的 superblock(區塊 0)整個清成 0
 *   9  確認掛載時改用備份的 superblock 並修好了區塊 0(而不是把磁碟當成新的重新格式化),檔案完好
 */
static void disk_stage_test(const char *cmd)
{
    struct wfs_stats s;
    struct stat st;

    wfs_stats(&s);
    if (cmdline_has(cmd, "disktest=1")) {
        KASSERT(s.mounted);
        vfs_unlink("/disk/persist");
        disk_write_file("/disk/persist", 9000, 3);
        KASSERT(vfs_chown("/disk/persist", 1000, 1001) == 0 && vfs_chmod("/disk/persist", 0640) == 0);
        kprintf("disktest 1: wrote /disk/persist (9000 bytes, owner 1000:1001, mode 640)\n");
    } else if (cmdline_has(cmd, "disktest=2")) {
        KASSERT(s.mounted && s.repairs == 0);
        disk_verify_file("/disk/persist", 9000, 3);
        /* 修改時間也存在磁碟上:是上一次開機時寫入的時間,不會比現在晚 */
        KASSERT(vfs_stat("/disk/persist", &st) == 0 && st.mtime > 1700000000ull &&
                st.mtime <= timer_epoch(NULL));
        /* 擁有者與權限也是:重新開機後從磁碟讀回來的和當初設定的相同 */
        KASSERT(st.uid == 1000 && st.gid == 1001 && st.mode == 0640);
        KASSERT(wfs_test_damage("persist") == 0);
        kprintf("disktest 2: /disk/persist survived the reboot; damaged the file system on purpose, "
                "exiting without a clean shutdown\n");
        arch_exit(0);
    } else if (cmdline_has(cmd, "disktest=3")) {
        KASSERT(s.mounted);
        kprintf("disktest 3: the mount-time check repaired %u problem(s)\n", s.repairs);
        KASSERT(s.repairs >= 4);
        disk_verify_file("/disk/persist", 9000, 3);
        KASSERT(vfs_stat("/disk/persist", &st) == 0 && st.nlink == 1);
        KASSERT(vfs_stat("/disk/ghost", &st) == -E_NOENT);
        KASSERT(wfs_check_now() == 0);
        kprintf("disktest 3: /disk/persist is intact; a second check found nothing\n");
    } else if (cmdline_has(cmd, "disktest=4")) {
        struct file *f;
        char buf[64];
        unsigned long n0 = blk_fault_count();

        KASSERT(s.mounted && s.repairs == 0 && !s.failed);
        /* 讀取失敗:清空快取之後讀檔案,得到 -E_IO;磁碟恢復之後同一個檔案照常可讀 */
        KASSERT(wfs_test_drop_cache() == 0);
        blk_fault_set(1, 0);
        KASSERT(vfs_open("/disk/persist", O_RDONLY, &f) == -E_IO ||
                (vfs_read(f, buf, sizeof(buf)) == -E_IO && (vfs_close(f), 1)));
        KASSERT(blk_fault_count() > n0);
        blk_fault_set(0, 0);
        disk_verify_file("/disk/persist", 9000, 3);
        wfs_stats(&s);
        KASSERT(!s.failed);                              /* 讀取失敗不影響檔案系統的狀態 */

        /* 寫入失敗:建立目錄得到 -E_IO,檔案系統轉為唯讀 */
        blk_fault_set(0, 1);
        KASSERT(vfs_mkdir("/disk/broken") == -E_IO);
        blk_fault_set(0, 0);
        wfs_stats(&s);
        KASSERT(s.failed);
        /* 磁碟恢復了也一樣:記憶體和磁碟已經對不上,不再接受任何修改 */
        KASSERT(vfs_mkdir("/disk/again") == -E_IO);
        KASSERT(vfs_unlink("/disk/persist") == -E_IO);
        KASSERT(vfs_open("/disk/persist", O_WRONLY | O_APPEND, &f) == 0);
        KASSERT(vfs_write(f, "x", 1) == -E_IO);
        vfs_close(f);
        KASSERT(vfs_open("/disk/new", O_CREAT | O_WRONLY, &f) == -E_IO);
        KASSERT(vfs_rename("/disk/persist", "/disk/moved") == -E_IO);
        disk_verify_file("/disk/persist", 9000, 3);      /* 讀取照常 */
        /* 超出範圍的讀寫要求:驅動程式拒絕,不會碰到別的地方 */
        struct blkdev *dev = arch_blk_probe_cached();
        KASSERT(dev && dev->rw(dev, dev->sectors, 8, buf, 0) != 0);
        KASSERT(dev->rw(dev, dev->sectors - 1, 8, buf, 0) != 0);
        kprintf("disktest 4: a read error only failed that one read; after a write error the file "
                "system refused every change but could still be read\n");
    } else if (cmdline_has(cmd, "disktest=5")) {
        KASSERT(s.mounted && !s.failed);
        /* 上一次因為寫入失敗沒有記「正常卸載」:這次掛載時一定做過檢查 */
        kprintf("disktest 5: after the write error the mount-time check ran (%u repair(s))\n", s.repairs);
        disk_verify_file("/disk/persist", 9000, 3);
        KASSERT(vfs_stat("/disk/broken", &st) == -E_NOENT && vfs_stat("/disk/again", &st) == -E_NOENT);
        KASSERT(wfs_check_now() == 0);
        KASSERT(vfs_mkdir("/disk/ok") == 0 && vfs_unlink("/disk/ok") == 0);      /* 又可以寫入了 */
        KASSERT(vfs_unlink("/disk/persist") == 0);
        kprintf("disktest 5: /disk/persist is intact and the file system is writable again\n");
    } else if (cmdline_has(cmd, "disktest=6")) {
        KASSERT(s.mounted && blackbox_enabled());
        blackbox_log(BB_NOTE, "stage 6 marker %d", 4711);
        kprintf("disktest 6: left a note in the black box; now panicking on purpose\n");
        panic("black box test panic %d", 77);
    } else if (cmdline_has(cmd, "disktest=7")) {
        struct bb_record r;
        int saw_panic = 0, saw_note = 0, saw_boot = 0;
        uint32_t prev_seq = 0;

        KASSERT(s.mounted && blackbox_enabled());
        /* 由新到舊走一遍:序號遞減;找得到上一次開機的註記、panic 的訊息、以及開機紀錄 */
        for (unsigned i = 0; blackbox_read(i, &r) == 0; i++) {
            KASSERT(i == 0 || r.seq < prev_seq);
            prev_seq = r.seq;
            KASSERT(r.len <= BB_TEXT_MAX && r.time > 1700000000ull);
            if (r.boot == s.mounts - 1) {
                saw_panic |= r.type == BB_PANIC && !memcmp(r.text, "black box test panic 77", 23);
                saw_note |= r.type == BB_NOTE && !memcmp(r.text, "stage 6 marker 4711", 19);
                saw_boot |= r.type == BB_BOOT;
            }
        }
        KASSERT(saw_panic && saw_note && saw_boot);
        /* 最新的一筆是這次的開機紀錄。把它在磁碟上翻轉一個位元:檢查碼不符,載入時被略過 */
        unsigned before = blackbox_count();
        KASSERT(blackbox_read(0, &r) == 0 && r.type == BB_BOOT && r.boot == s.mounts);
        KASSERT(blackbox_test_corrupt_newest() == 0);
        KASSERT(blackbox_count() == before - 1);
        KASSERT(blackbox_read(0, &r) == 0 && r.boot == s.mounts - 1);   /* 現在最新的是上一次開機留下的 */
        blackbox_log(BB_NOTE, "stage 7 done");
        KASSERT(blackbox_read(0, &r) == 0 && !memcmp(r.text, "stage 7 done", 12));
        kprintf("disktest 7: the black box kept the note and the panic message across the reboot; "
                "a record with one flipped bit was rejected\n");
    } else if (cmdline_has(cmd, "disktest=8")) {
        static uint8_t zero[512];
        struct blkdev *dev = arch_blk_probe_cached();

        KASSERT(s.mounted && s.checksums && !s.sb_restored);
        vfs_unlink("/disk/sbtest");
        disk_write_file("/disk/sbtest", 20000, 9);
        blackbox_log(BB_SHUTDOWN, "clean shutdown before the superblock test");
        wfs_shutdown();                                  /* 正常卸載:兩份 superblock 都是最新的 */
        /* 把區塊 0 的 8 個磁區全部清成 0:最壞的情況,看起來就像一顆全新的磁碟 */
        for (unsigned i = 0; i < 8; i++)
            KASSERT(dev->rw(dev, i, 1, zero, 1) == 0);
        kprintf("disktest 8: wrote /disk/sbtest, unmounted, then wiped the primary superblock\n");
        arch_exit(0);
    } else if (cmdline_has(cmd, "disktest=9")) {
        KASSERT(s.mounted && s.checksums && s.sb_restored);
        disk_verify_file("/disk/sbtest", 20000, 9);      /* 沒有被重新格式化:檔案還在而且正確 */
        KASSERT(wfs_check_now() == 0);
        KASSERT(vfs_unlink("/disk/sbtest") == 0);
        kprintf("disktest 9: the backup superblock was used and the primary repaired; "
                "/disk/sbtest is intact\n");
    } else if (cmdline_has(cmd, "disktest=10")) {
        /* 間接區塊讀不到時刪除檔案:不可以把它底下的資料區塊弄丟。
           預期:什麼都不釋放、檔案系統轉為唯讀;下一次開機的檢查把整個檔案的空間收回來(第 11 階段) */
        struct wfs_stats before, after;
        struct file *f;
        uint32_t base;

        KASSERT(s.mounted && s.checksums && !s.failed);
        vfs_unlink("/disk/indbase");
        vfs_unlink("/disk/indfile");
        KASSERT(vfs_open("/disk/indbase", O_CREAT | O_WRONLY | O_TRUNC, &f) == 0);
        KASSERT(vfs_write(f, &base, sizeof(base)) == sizeof(base));      /* 先佔好位置 */
        vfs_close(f);
        KASSERT(vfs_sync() == 0);
        wfs_stats(&before);
        base = before.free_blocks;                       /* 基準:indfile 還不存在時的可用區塊數 */
        KASSERT(vfs_open("/disk/indbase", O_WRONLY, &f) == 0);
        KASSERT(vfs_write(f, &base, sizeof(base)) == sizeof(base));
        vfs_close(f);
        disk_write_file("/disk/indfile", 12 * PAGE_SIZE, 11);            /* 12 個區塊:用到間接區塊 */
        KASSERT(wfs_test_drop_cache() == 0);
        wfs_stats(&before);
        KASSERT(before.free_blocks == base - 13);        /* 12 個資料 + 1 個間接 */
        KASSERT(vfs_open("/disk/indfile", O_RDONLY, &f) == 0);
        uint32_t ind = wfs_test_file_block(vfs_file_inode(f), 0xFFFFFFFFu);
        vfs_close(f);
        KASSERT(ind != 0 && wfs_test_drop_cache() == 0);
        KASSERT(blk_fault_flip((uint64_t)ind * 8, 100, 5) == 0);
        KASSERT(vfs_unlink("/disk/indfile") == 0);       /* 名稱拿掉了(刪除本身成功),但內容釋放不了 */
        wfs_stats(&after);
        KASSERT(after.failed && after.csum_errors >= 1);
        KASSERT(after.free_blocks == before.free_blocks);                /* 一個區塊都沒有被釋放 */
        KASSERT(vfs_mkdir("/disk/no") == -E_IO);         /* 已經轉為唯讀 */
        kprintf("disktest 10: the indirect block of a 12-block file was unreadable when the file was "
                "deleted; nothing was freed and the file system went read-only\n");
    } else if (cmdline_has(cmd, "disktest=11")) {
        struct file *f;
        uint32_t base = 0;

        KASSERT(s.mounted && !s.failed && s.repairs >= 1);               /* 開機時的檢查處理過了 */
        KASSERT(vfs_stat("/disk/indfile", &st) == -E_NOENT);
        KASSERT(vfs_open("/disk/indbase", O_RDONLY, &f) == 0);
        KASSERT(vfs_read(f, &base, sizeof(base)) == sizeof(base));
        vfs_close(f);
        KASSERT(s.free_blocks == base);                  /* 13 個區塊全部收回來了,沒有遺失 */
        KASSERT(wfs_check_now() == 0);
        KASSERT(vfs_unlink("/disk/indbase") == 0);
        kprintf("disktest 11: the mount-time check reclaimed all 13 blocks of the deleted file\n");
    }
}

/* ------------------------------------------------------------------ */
/* 檔案系統的示範與測試(kernel 內部直接呼叫 VFS)                        */
/* ------------------------------------------------------------------ */

/* 遞迴列出目錄的內容(印在同一行) */
static void fs_list(const char *path, int depth)
{
    struct file *d;
    struct dirent e;

    if (depth > 4 || vfs_open(path, O_RDONLY, &d) != 0)
        return;
    while (vfs_readdir(d, &e) == 0) {
        char sub[FS_PATH_MAX + 1];
        size_t n = 0;
        for (const char *p = path; *p && n < FS_PATH_MAX; p++)
            sub[n++] = *p;
        if (n > 1 && n < FS_PATH_MAX)
            sub[n++] = '/';
        for (const char *p = e.name; *p && n < FS_PATH_MAX; p++)
            sub[n++] = *p;
        sub[n] = '\0';
        kprintf(" %s%s", sub, e.type == I_DIR ? "/" : "");
        if (e.type == I_DIR)
            fs_list(sub, depth + 1);
    }
    vfs_close(d);
}

#define FS_WORKERS 4
#define FS_FILES   25
#define FS_RECORD  32u                       /* 共用紀錄檔中每一筆的長度 */

/* 多條執行緒同時在同一個目錄建立、寫入、讀回、刪除各自的檔案,
   並各自往同一個紀錄檔附加固定長度的紀錄 */
static void fs_worker(void *arg)
{
    int id = (int)(uintptr_t)arg;
    char path[32] = "/t/w0-00", rec[FS_RECORD], back[600], data[600];
    struct file *f, *log;

    KASSERT(vfs_open("/t/log", O_WRONLY | O_APPEND, &log) == 0);
    for (unsigned i = 0; i < sizeof(data); i++)
        data[i] = (char)(id * 31 + i);
    for (int n = 0; n < FS_FILES; n++) {
        path[4] = (char)('0' + id);
        path[6] = (char)('0' + n / 10);
        path[7] = (char)('0' + n % 10);
        KASSERT(vfs_open(path, O_CREAT | O_RDWR, &f) == 0);
        KASSERT(vfs_write(f, data, sizeof(data)) == (long)sizeof(data));
        KASSERT(vfs_pread(f, back, sizeof(back), 0) == (long)sizeof(back));
        KASSERT(memcmp(data, back, sizeof(data)) == 0);
        vfs_close(f);
        KASSERT(vfs_unlink(path) == 0);

        /* 一筆紀錄全部是同一個字元:之後檢查時,混到別人的內容就看得出來 */
        memset(rec, 'A' + id, sizeof(rec));
        KASSERT(vfs_write(log, rec, sizeof(rec)) == (long)sizeof(rec));
    }
    vfs_close(log);
}

static void fs_demo(void)
{
    struct file *f;
    struct stat st;
    static char buf[10000], back[10000];
    unsigned long pages0 = ramfs_pages();
    struct kheap_stats h0, h1;

    sched_trim();
    kheap_stats(&h0);
    size_t pool0 = sched_pool_bytes();

    kprintf("fs: tree:");
    fs_list("/", 0);
    kprintf("\n");

    /* 1. 讀取開機檔案包帶來的檔案 */
    KASSERT(vfs_open("/etc/motd", O_RDONLY, &f) == 0);
    long n = vfs_read(f, buf, 80);
    KASSERT(n > 0);
    for (long i = 0; i < n; i++)
        if (buf[i] == '\n')
            buf[i] = '\0';
    buf[n] = '\0';
    kprintf("fs: /etc/motd: \"%s\"\n", buf);
    KASSERT(vfs_write(f, "x", 1) == -E_BADF);         /* 唯讀開啟 */
    vfs_close(f);

    /* 2. 建立、寫入超過兩頁的資料、讀回比對 */
    KASSERT(vfs_mkdir("/t") == 0);
    KASSERT(vfs_mkdir("/t") == -E_EXIST);
    for (unsigned i = 0; i < sizeof(buf); i++)
        buf[i] = (char)(i * 13 + 1);
    KASSERT(vfs_open("/t/a", O_CREAT | O_RDWR, &f) == 0);
    KASSERT(vfs_write(f, buf, sizeof(buf)) == (long)sizeof(buf));
    KASSERT(vfs_pread(f, back, sizeof(back), 0) == (long)sizeof(back));
    KASSERT(memcmp(buf, back, sizeof(buf)) == 0);
    KASSERT(vfs_read(f, back, 10) == 0);              /* 寫完後位置在結尾:讀不到東西 */
    KASSERT(vfs_stat("/t/a", &st) == 0 && st.type == I_FILE && st.size == sizeof(buf));
    KASSERT(ramfs_pages() == pages0 + 3);             /* 10000 位元組 = 3 頁 */
    vfs_close(f);

    /* 3. 附加:以 O_APPEND 開啟時,每次寫入都接在檔案最後 */
    KASSERT(vfs_open("/t/b", O_CREAT | O_WRONLY | O_APPEND, &f) == 0);
    KASSERT(vfs_write(f, "head", 4) == 4);
    vfs_close(f);
    KASSERT(vfs_open("/t/b", O_RDWR | O_APPEND, &f) == 0);
    KASSERT(vfs_write(f, "tail", 4) == 4);
    KASSERT(vfs_pread(f, back, 16, 0) == 8 && memcmp(back, "headtail", 8) == 0);
    vfs_close(f);

    /* 4. 開著的時候被刪除:名稱立刻消失,內容留到關閉為止 */
    KASSERT(vfs_open("/t/a", O_RDONLY, &f) == 0);
    KASSERT(vfs_unlink("/t/a") == 0);
    KASSERT(vfs_stat("/t/a", &st) == -E_NOENT);
    KASSERT(vfs_pread(f, back, 100, 5000) == 100 && memcmp(back, buf + 5000, 100) == 0);
    KASSERT(ramfs_pages() == pages0 + 4);             /* a 的 3 頁還在,加上 b 的 1 頁 */
    vfs_close(f);
    KASSERT(ramfs_pages() == pages0 + 1);             /* 關閉後才真的釋放 */

    /* 5. O_TRUNC:重新開啟並清空 */
    KASSERT(vfs_open("/t/b", O_WRONLY | O_TRUNC, &f) == 0);
    KASSERT(vfs_size(f) == 0 && ramfs_pages() == pages0);
    vfs_close(f);

    /* 6. 錯誤情況 */
    KASSERT(vfs_open("/nope", O_RDONLY, &f) == -E_NOENT);
    KASSERT(vfs_open("/etc/motd/x", O_RDONLY, &f) == -E_NOTDIR);
    KASSERT(vfs_open("/t", O_WRONLY, &f) == -E_ISDIR);
    KASSERT(vfs_unlink("/t") == -E_NOTEMPTY);
    KASSERT(vfs_unlink("/") == -E_INVAL);
    KASSERT(vfs_mkdir("/t/..") == -E_EXIST);
    KASSERT(vfs_stat("/t/../etc/./motd", &st) == 0);  /* "." 與 ".." */
    {
        char longname[FS_NAME_MAX + 8];
        longname[0] = '/';
        memset(longname + 1, 'x', sizeof(longname) - 2);
        longname[sizeof(longname) - 1] = '\0';
        KASSERT(vfs_mkdir(longname) == -E_NAMETOOLONG);
    }

    /* 7. 多條執行緒(在多顆 CPU 上)同時操作同一個目錄與同一個紀錄檔 */
    int tid[FS_WORKERS];
    KASSERT(vfs_open("/t/log", O_CREAT | O_WRONLY, &f) == 0);
    vfs_close(f);
    uint64_t t0 = timer_ticks();
    for (int i = 0; i < FS_WORKERS; i++)
        tid[i] = thread_create("fs-worker", fs_worker, (void *)(uintptr_t)i);
    for (int i = 0; i < FS_WORKERS; i++)
        thread_join(tid[i]);
    /* 紀錄檔:總長度正確,每一筆都完整(32 個相同的字元),每條執行緒的筆數正確 */
    unsigned count[FS_WORKERS] = { 0 };
    KASSERT(vfs_open("/t/log", O_RDONLY, &f) == 0);
    KASSERT(vfs_size(f) == (uint64_t)FS_WORKERS * FS_FILES * FS_RECORD);
    char rec[FS_RECORD];
    while (vfs_read(f, rec, sizeof(rec)) == (long)sizeof(rec)) {
        for (unsigned i = 1; i < sizeof(rec); i++)
            KASSERT(rec[i] == rec[0]);
        KASSERT(rec[0] >= 'A' && rec[0] < 'A' + FS_WORKERS);
        count[rec[0] - 'A']++;
    }
    vfs_close(f);
    for (int i = 0; i < FS_WORKERS; i++)
        KASSERT(count[i] == FS_FILES);
    kprintf("fs: %d threads x %d files (create, write, read back, delete) plus a shared "
            "append-only log: all intact, %llu ticks\n", FS_WORKERS, FS_FILES,
            (unsigned long long)(timer_ticks() - t0));

    /* 8. 全部刪掉之後,佔用的記憶體要回到原本的樣子 */
    KASSERT(vfs_unlink("/t/log") == 0 && vfs_unlink("/t/b") == 0 && vfs_unlink("/t") == 0);
    sched_trim();
    kheap_stats(&h1);
    KASSERT(ramfs_pages() == pages0);
    KASSERT(h1.free_bytes + (sched_pool_bytes() - pool0) == h0.free_bytes + (h1.heap_bytes - h0.heap_bytes));
    kprintf("fs: selftest ok (all files removed, memory returned)\n");
}

/* ------------------------------------------------------------------ */
/* 管線的測試(kernel 內部直接呼叫 VFS)                                  */
/* ------------------------------------------------------------------ */
#define PIPE_TOTAL 50000u                    /* 遠大於管線的緩衝區(4KB):寫入者一定會被擋下來等 */

static void pipe_writer(void *arg)
{
    struct file *wr = arg;
    char chunk[700];

    for (unsigned done = 0; done < PIPE_TOTAL; ) {
        unsigned n = PIPE_TOTAL - done < sizeof(chunk) ? PIPE_TOTAL - done : sizeof(chunk);
        for (unsigned i = 0; i < n; i++)
            chunk[i] = (char)((done + i) * 7 + 5);
        KASSERT(vfs_write(wr, chunk, n) == (long)n);
        done += n;
    }
    vfs_close(wr);                           /* 寫完就關閉寫端:讀取者會因此讀到結束 */
}

static void pipe_demo(void)
{
    struct file *rd, *wr;
    char buf[300];
    struct kheap_stats h0, h1;

    sched_trim();
    kheap_stats(&h0);
    size_t pool0 = sched_pool_bytes();

    /* 1. 一條執行緒寫、另一條讀,資料量遠大於緩衝區:順序與內容都必須正確 */
    KASSERT(vfs_pipe(&rd, &wr) == 0);
    KASSERT(vfs_read(wr, buf, 1) == -E_BADF && vfs_write(rd, buf, 1) == -E_BADF);   /* 方向固定 */
    uint64_t t0 = timer_ticks();
    int tid = thread_create("pipe-writer", pipe_writer, wr);
    unsigned got = 0;
    long n;
    while ((n = vfs_read(rd, buf, sizeof(buf))) > 0) {
        for (long i = 0; i < n; i++)
            KASSERT(buf[i] == (char)((got + (unsigned)i) * 7 + 5));
        got += (unsigned)n;
    }
    KASSERT(n == 0 && got == PIPE_TOTAL);    /* 寫端關閉後讀到 0,而且一個位元組都沒少 */
    thread_join(tid);
    kprintf("pipe: %u bytes through a 4 KB pipe between two threads, in order, %llu ticks\n",
            got, (unsigned long long)(timer_ticks() - t0));
    vfs_close(rd);

    /* 2. 沒有人讀的管線:寫入要回報錯誤,而不是永遠等下去 */
    KASSERT(vfs_pipe(&rd, &wr) == 0);
    vfs_close(rd);
    KASSERT(vfs_write(wr, "x", 1) == -E_PIPE);
    vfs_close(wr);

    /* 3. 寫端關閉後,緩衝區裡剩下的資料仍然讀得到,讀完才是結束 */
    KASSERT(vfs_pipe(&rd, &wr) == 0);
    KASSERT(vfs_write(wr, "left over", 9) == 9);
    vfs_close(wr);
    KASSERT(vfs_read(rd, buf, sizeof(buf)) == 9 && memcmp(buf, "left over", 9) == 0);
    KASSERT(vfs_read(rd, buf, sizeof(buf)) == 0);
    vfs_close(rd);

    sched_trim();
    kheap_stats(&h1);
    KASSERT(h1.free_bytes + (sched_pool_bytes() - pool0) == h0.free_bytes + (h1.heap_bytes - h0.heap_bytes));
    kprintf("pipe: selftest ok\n");
}

/* 傳給使用者程式的 mode;數值必須與 user/init.c 一致 */
#include "uapi/selftest.h"       /* 測試模式的編號:和 user/init.c 共用同一份定義 */

/* 以測試編號 mode 啟動 /bin/init:編號轉成字串,當作 argv[1] 傳給它 */
static int spawn_init(long mode)
{
    char num[12];
    int n = 0;
    const char *argv[] = { "init", num, NULL };

    if (mode >= 10)
        num[n++] = (char)('0' + mode / 10);
    num[n++] = (char)('0' + mode % 10);
    num[n] = '\0';
    return proc_spawn("/bin/init", argv);
}

/* 啟動一個行程、等它結束,確認結束碼符合預期 */
static void run_user(const char *what, long mode, int expect)
{
    int pid = spawn_init(mode);
    KASSERT(pid > 0);
    uint64_t t0 = timer_ticks();
    int code = proc_wait(pid);
    kprintf("user: %s -> exit code %d (%u ms)\n", what, code, (unsigned)(timer_ticks() - t0));
    if (code != expect)
        panic("user test \"%s\": exit code %d, expected %d", what, code, expect);
}

static void user_demo(void)
{
    sched_trim();
    size_t free0 = pmm_free_count() + vm_table_pages();
    struct kheap_stats h0, h1;
    kheap_stats(&h0);
    size_t pool0 = sched_pool_bytes();       /* 執行緒結構留在池子裡不歸還,核對時要扣掉 */

    kprintf("user: running a program in user mode\n");
    run_user("hello, 2 threads, exit(42)", UMODE_HELLO, 42);

    /* 檔案系統呼叫:建立、讀寫、列目錄、刪除,以及位址與權限的檢查 */
    run_user("file system calls", UMODE_FS, 9);
    /* 程式檔不存在、或內容不是合法的程式:建立行程要失敗,而不是執行到一半出事 */
    KASSERT(proc_spawn("/bin/nope", NULL) == -1);
    KASSERT(proc_spawn("/etc/motd", NULL) == -1);

    /* 參數傳遞:kernel 直接啟動 /bin/echo,它的結束碼是收到的參數個數 */
    {
        const char *argv[] = { "echo", "  [user] /bin/echo started by the kernel with", "3 arguments", NULL };
        int pid = proc_spawn("/bin/echo", argv);
        KASSERT(pid > 0 && proc_wait(pid) == 3);
    }
    /* 使用者程式自己建立行程、等待、重新導向輸出;以及各種應該被拒絕的情況 */
    run_user("spawn, wait, arguments, redirection", UMODE_PROC, 12);
    /* exec:行程把自己換成 /bin/echo。結束碼來自 echo(3 個參數),不是原本的程式 */
    run_user("exec replaces the program", UMODE_EXEC, 3);
    run_user("exec and fork with a second thread are refused", UMODE_EXEC_THREADS, 13);

    /* fork:複製行程。記憶體以寫入時複製的方式共用,所以順便看複製了幾頁 */
    {
        unsigned long c0 = vm_cow_copies(), r0 = vm_cow_reuses();
        run_user("fork (copy-on-write)", UMODE_FORK, 14);
        kprintf("user: copy-on-write during that test: %lu pages copied, %lu times a page "
                "was reused without copying\n", vm_cow_copies() - c0, vm_cow_reuses() - r0);
    }
    /* 管線:父子行程之間傳資料、寫端全關閉後讀到結束、把別的程式的輸出接進管線 */
    run_user("pipes between processes", UMODE_PIPE, 16);
    /* 父子關係與訊號。其中一項會留下一個沒有人 wait 的孫行程,它要 5ms 後才結束,
       所以多等一下,讓最後的記憶體核對能確認它被自動回收了 */
    run_user("parent/child, signals, process groups", UMODE_SIGNAL, 17);
    /* 背景執行與 ps 的基礎:不等待的 waitpid、列出行程、/dev/null */
    run_user("waitpid without waiting, process list, /dev/null", UMODE_JOBS, 18);
    /* 工作控制的基礎:暫停、繼續、暫停的回報。最後一項留下一個被 kernel 終止的孫行程 */
    run_user("stop and continue (job control)", UMODE_STOP, 19);
    /* 磁碟(有掛載的話):把程式複製到磁碟上再執行、查詢用量、sync */
    {
        struct wfs_stats ws;
        wfs_stats(&ws);
        if (ws.mounted)
            run_user("run a program from the disk, statfs, sync", UMODE_DISK, 20);
    }
    /* 檔案介面的另一半:讀寫位置、改名、硬連結、工作目錄(記憶體檔案系統與磁碟上各測一次) */
    run_user("lseek, rename, link, working directory", UMODE_FILEOPS, 22);
    {
        /* 改名與硬連結動到磁碟上的目錄與名稱數:做完之後整個檔案系統必須仍然一致 */
        struct wfs_stats ws;
        wfs_stats(&ws);
        if (ws.mounted)
            KASSERT(wfs_check_now() == 0);
    }
    /* 使用者 heap(sbrk:用到才配置、縮小時歸還)與函式庫(malloc、printf、字串) */
    run_user("user heap and C library", UMODE_HEAP, 23);
    /* 訊號處理函式:暫存器存到使用者 stack、執行、還原;忽略;打斷等待;捏造的 sigreturn */
    run_user("signal handlers", UMODE_SIGHANDLER, 24);
    /* 日期時間:硬體時鐘、秒數與年月日的換算、檔案的修改時間 */
    run_user("date and time", UMODE_TIME, 25);
    /* 使用者與權限:檔案的擁有者與權限位元(記憶體與磁碟各測一次)、特權只能減少、送訊號的限制 */
    run_user("users and permissions", UMODE_PERM, 26);
    /* 記憶體配額(超過的要求當場失敗)與匿名對映 */
    run_user("memory quota and mmap", UMODE_MEMQUOTA, 27);
    /* 週期性的即時工作:參數檢查、負載上限、需要特權、準時 */
    run_user("periodic real-time tasks", UMODE_RT, 29);
    /* 三重執行加投票:對其中一份注入位元翻轉、讓一份卡住、讓兩份都出錯 */
    run_user("triple execution with voting", UMODE_TMR, 30);
    /* 網路(有網路卡的話):ping、UDP socket、以 TFTP 實際收送一個檔案 */
    if (boot_nic)
        run_user("network: ping and UDP", UMODE_NET, 28);
    /* top 的基礎:系統狀態、行程的記憶體用量、主控台的輸入模式 */
    run_user("system information for top", UMODE_SYSINFO, 21);
    thread_sleep(30);
    /* stack 自動成長:用掉約 180KB 的 stack(一開始只配置 16KB) */
    run_user("stack grows on demand", UMODE_STACK_GROW, 15);

    /* shell 執行一份 script:建立目錄、重新導向輸出到檔案、找不到的命令、最後 exit 7。
       除了結束碼,也檢查它留下的檔案內容 */
    {
        const char *argv[] = { "sh", "/etc/test.sh", NULL };
        struct file *f;
        char buf[80];
        /* 最後一行來自管線:cat /etc/motd | cat | wc(2 行、18 個字、104 位元組) */
        static const char want[] = "first line\nsecond line, with  two  spaces\n2 18 104\n";
        int pid = proc_spawn("/bin/sh", argv);
        KASSERT(pid > 0);
        int code = proc_wait(pid);
        kprintf("user: shell script /etc/test.sh -> exit code %d\n", code);
        KASSERT(code == 7);
        KASSERT(vfs_open("/work/result", O_RDONLY, &f) == 0);
        long n = vfs_read(f, buf, sizeof(buf));
        vfs_close(f);
        KASSERT(n == (long)sizeof(want) - 1 && memcmp(buf, want, (size_t)n) == 0);
        KASSERT(vfs_unlink("/work/result") == 0 && vfs_unlink("/work") == 0);
    }

    /* 正在等主控台輸入的行程也要能被終止:啟動一個互動式的 shell(沒有人打字,
       它會一直睡在讀取裡),過一下子把它終止,確認它真的結束了 */
    {
        int pid = proc_spawn("/bin/sh", NULL);
        KASSERT(pid > 0);
        thread_sleep(10);
        KASSERT(proc_kill(pid, 98) == 0);
        int code = proc_wait(pid);
        kprintf("\nuser: shell blocked on console input, killed -> exit code %d\n", code);
        KASSERT(code == 98);
    }

    /* 使用者模式的 mutex(futex):3 條執行緒搶同一把鎖,總和必須正確 */
    run_user("user-mode mutex (futex), 3 threads", UMODE_FUTEX, 11);

    /* 以下每一個都是使用者程式做錯事。預期:只有那個行程被終止(結束碼 -1),
       kernel 繼續正常運作並執行下一個測試 */
    run_user("read kernel memory", UMODE_READ_KERNEL, PROC_KILLED_FAULT);
    run_user("write to its own code", UMODE_WRITE_TEXT, PROC_KILLED_FAULT);
    run_user("privileged instruction", UMODE_PRIVILEGED, PROC_KILLED_FAULT);
    run_user("NULL dereference", UMODE_NULL, PROC_KILLED_FAULT);
    run_user("user stack overflow", UMODE_STACK, PROC_KILLED_FAULT);

    /* 系統呼叫的參數檢查:不合法的指標與編號要被拒絕,行程本身不受影響 */
    run_user("kernel pointer passed to write()", UMODE_BAD_POINTER, 7);
    run_user("unknown system call number", UMODE_BAD_SYSCALL, 9);

    /* 搶佔與強制終止:行程陷入無窮迴圈、從不呼叫 kernel。
       計時器中斷仍會把 CPU 拿回來,main 才有機會執行並終止它 */
    int pid = spawn_init(UMODE_SPIN);
    KASSERT(pid > 0);
    thread_sleep(20);
    KASSERT(proc_kill(pid, 99) == 0);
    int code = proc_wait(pid);
    kprintf("user: infinite loop, killed after 20 ms -> exit code %d\n", code);
    KASSERT(code == 99);

    /* 隔離:三個行程同時執行同一個程式,各自寫入「同一個虛擬位址」的變數。
       它們的頁表把這個位址對映到不同的實體頁,所以互不影響 */
    int p[3];
    for (int i = 0; i < 3; i++) {
        p[i] = spawn_init(UMODE_ISOLATION);
        KASSERT(p[i] > 0);
    }
    for (int i = 0; i < 3; i++)
        KASSERT(proc_wait(p[i]) == 0);
    kprintf("user: 3 processes wrote the same virtual address without interfering\n");

    KASSERT(proc_wait(12345) == -1);         /* 不存在的 pid */

    /* 所有行程結束後,它們的頁面、頁表、stack 都必須歸還 */
    thread_sleep(2);
    sched_trim();
    kheap_stats(&h1);
    KASSERT(sched_thread_count() == 1 + resident_threads());   /* main,加上網路的接收執行緒(有網路卡時) */
    /* heap 向頁面配置器要來的頁不會歸還(見 kmalloc.c),所以這段期間 heap 若長大了
       (例如管線的緩衝區第一次配置),要把那部分算進去:那些頁現在是 heap 的空閒空間 */
    size_t heap_grow = h1.heap_bytes - h0.heap_bytes;
    KASSERT(pmm_free_count() + vm_table_pages() == free0 - heap_grow / PAGE_SIZE);
    KASSERT(h1.free_bytes + (sched_pool_bytes() - pool0) == h0.free_bytes + heap_grow);
    kprintf("user: all processes exited, memory reclaimed\n");
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

static void churn_worker(void *arg)
{
    (void)arg;
}

static void sched_stress(void)
{
    sched_trim();                            /* 先清掉前面示範留在快取裡的 stack,基準才準 */
    size_t free0 = pmm_free_count() + vm_table_pages();
    size_t used0 = pmm_free_count();
    unsigned long sw0 = sched_switch_count();
    uint64_t t0 = timer_ticks();
    unsigned created = 0;
    struct kheap_stats h0, h1;
    kheap_stats(&h0);
    size_t pool0 = sched_pool_bytes();       /* 執行緒結構留在池子裡不歸還,核對時要扣掉 */

    for (unsigned i = 0; i < STRESS_THREADS; i++) {
        if (thread_create("stress", stress_worker, NULL) < 0)
            break;
        created++;
    }
    kprintf("stress: created %u threads, %zu KB used (stacks + thread structs)\n", created,
            (used0 - pmm_free_count()) * (PAGE_SIZE / 1024));
    for (unsigned i = 0; i < created; i++)
        sem_wait(&stress_sem);               /* 每等到一次代表一條完成 */
    thread_sleep(10);                        /* post 之後它們還要一點時間真正結束並被回收 */
    kprintf("stress: all finished in %llu ticks, %lu context switches\n",
            (unsigned long long)(timer_ticks() - t0), sched_switch_count() - sw0);
    KASSERT(created == STRESS_THREADS && sched_thread_count() == 1 + resident_threads());

    /* 第二階段:一條接一條地建立並等待結束(同時只有一條存活)。
       這是 stack 快取發揮作用的情況:每次都重複使用同一條 stack,
       不必重新配置頁面、移除保護頁的對映、清 TLB */
    t0 = timer_ticks();
    for (unsigned i = 0; i < STRESS_THREADS; i++) {
        int tid = thread_create("churn", churn_worker, NULL);
        KASSERT(tid > 0);
        thread_join(tid);
    }
    kprintf("stress: %u sequential create+join in %llu ticks\n", STRESS_THREADS,
            (unsigned long long)(timer_ticks() - t0));

    /* 第三階段:連續建立並結束 300 個行程。位址空間編號(ASID)只有 254 個可用,
       所以一定會重複使用;每個行程都必須正常執行,不受前一個使用同編號者的殘留影響 */
    t0 = timer_ticks();
    for (int i = 0; i < 300; i++) {
        int pid = spawn_init(UMODE_BAD_SYSCALL);
        KASSERT(pid > 0 && proc_wait(pid) == 9);
    }
    kprintf("stress: 300 sequential processes in %llu ticks\n",
            (unsigned long long)(timer_ticks() - t0));
    thread_sleep(2);
    /* stack 必須全部歸還。heap 為了放 1000 個 struct thread 而向 pmm 多要的頁
       不會歸還(kmalloc 的已知限制),所以要扣掉;但那些空間在 heap 內必須都已空閒 */
    kheap_stats(&h1);
    size_t heap_grow = h1.heap_bytes - h0.heap_bytes;
    sched_trim();
    KASSERT(pmm_free_count() + vm_table_pages() == free0 - heap_grow / PAGE_SIZE);
    KASSERT(h1.free_bytes + (sched_pool_bytes() - pool0) == h0.free_bytes + heap_grow);
    kprintf("stress: ok (stacks reclaimed, heap grew by %zu KB)\n", heap_grow / 1024);
}

/*
 * crash=stack:遞迴到超出 16KB 的 stack。stack 下方的保護頁沒有對映,
 * 一碰到就觸發例外,應在溢位的當下被攔下並回報為 KERNEL STACK OVERFLOW。
 */
static int overflow_recurse(int depth)
{
    volatile char pad[512];
    for (unsigned i = 0; i < sizeof(pad); i++)
        pad[i] = (char)depth;
    if (depth == 0) {
        thread_yield();                      /* 不應到達:途中就該碰到保護頁 */
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

/* crash=wdog 用:關著中斷空轉。這顆 CPU 從此不再處理任何中斷 */
static void spin_irqs_off(void *arg)
{
    (void)arg;
    arch_irq_disable();
    for (;;)
        ;
}

/* crash=smp 用:永遠忙碌的執行緒 */
static void spin_forever(void *arg)
{
    (void)arg;
    for (;;)
        ;
}

static void sched_crash_test(const char *cmd)
{
    if (cmdline_has(cmd, "crash=stack")) {
        kprintf("overflowing a thread stack...\n");
        thread_create("overflow", overflow_worker, NULL);
        thread_sleep(100);
    } else if (cmdline_has(cmd, "crash=mainstack")) {
        /* 同樣的溢位,但發生在 main 執行緒使用的開機 stack(它也有保護頁) */
        kprintf("overflowing the boot stack...\n");
        overflow_recurse(40);
    } else if (cmdline_has(cmd, "crash=thread")) {
        thread_create("faulty", fault_worker, NULL);
        thread_sleep(100);
    } else if (cmdline_has(cmd, "crash=uaccess") || cmdline_has(cmd, "crash=uexec")) {
        /*
         * kernel 直接讀取 / 執行使用者的頁。這在正常的程式裡不該發生(kernel 一律
         * 透過 copy_from_user 存取使用者資料),一旦發生通常是被惡意的指標誘導。
         * 硬體保護應該擋下:
         *   讀取:i386 SMAP、AArch64 PAN、RISC-V 的 sstatus.SUM = 0
         *   執行:i386 SMEP、AArch64 PXN、RISC-V 的 S-mode 不可執行 U 頁
         */
        struct vm_space *vs = vm_space_create();
        void *page = pmm_alloc();
        KASSERT(vs && page);
        memset(page, 0, PAGE_SIZE);
        int exec = cmdline_has(cmd, "crash=uexec");
        KASSERT(vm_space_map(vs, USER_BASE, (uintptr_t)page,
                             exec ? (VM_READ | VM_EXEC) : (VM_READ | VM_WRITE)) == 0);
        thread_set_space(vs);
        if (exec) {
            kprintf("kernel jumping to a user page...\n");
            ((void (*)(void))(uintptr_t)USER_BASE)();
            kprintf("execution returned?!\n");
        } else {
            kprintf("kernel reading a user page directly...\n");
            volatile int *p = (volatile int *)(uintptr_t)USER_BASE;
            kprintf("read %d: NOT blocked on this CPU\n", *p);
        }
    } else if (cmdline_has(cmd, "crash=relock")) {
        /* 同一條執行緒對同一個 mutex 上鎖兩次:不偵測的話會自己等自己 */
        static struct mutex m = MUTEX_INIT;
        kprintf("locking a mutex twice...\n");
        mutex_lock(&m);
        mutex_lock(&m);
    } else if (cmdline_has(cmd, "crash=spinsleep")) {
        /* 持有 spinlock 時切換執行緒:別人會關著中斷一直等這把鎖,所以禁止 */
        static struct spinlock l = SPINLOCK_INIT("test");
        kprintf("yielding the CPU while holding a spinlock...\n");
        unsigned long flags = spin_lock_irqsave(&l);
        thread_yield();
        spin_unlock_irqrestore(&l, flags);
    } else if (cmdline_has(cmd, "crash=spin")) {
        /* 同一顆 CPU 對同一把 spinlock 上鎖兩次:不偵測的話會自己等自己,而且中斷是關的 */
        static struct spinlock l = SPINLOCK_INIT("test");
        kprintf("acquiring a spinlock twice...\n");
        unsigned long flags = spin_lock_irqsave(&l);
        (void)spin_lock_irqsave(&l);
        spin_unlock_irqrestore(&l, flags);
    }
}

/*
 * 多核心排程的示範與測試(其他 CPU 已加入排程之後執行):
 *   1. 每顆 CPU 都真的在執行執行緒:建立比 CPU 數更多的忙碌執行緒,
 *      記錄它們各自在哪些 CPU 上跑過,最後每一顆 CPU 都應該出現過
 *   2. 執行緒會在 CPU 之間移動(同一條執行緒先後出現在不同的 CPU 上)
 *   3. kernel 的 mutex 在真正同時執行時仍然正確:多條執行緒同時遞增同一個計數器
 */
#define SMP_WORKERS 8
static volatile unsigned smp_seen[SMP_WORKERS];      /* 每條執行緒跑過的 CPU(位元遮罩) */
static volatile unsigned long smp_spins[SMP_WORKERS];
static struct mutex smp_mtx = MUTEX_INIT;
static volatile unsigned long smp_counter;
/* 每條執行緒遞增幾次。爭用時每次上鎖都可能睡下去再被叫醒,不宜太多 */
#define SMP_ADDS 1000u

static void smp_worker(void *arg)
{
    unsigned i = (unsigned)(uintptr_t)arg;
    uint64_t end = timer_ticks() + 30;       /* 忙 30ms */

    while (timer_ticks() < end) {
        smp_seen[i] |= 1u << sched_cpu_id();
        smp_spins[i]++;
    }
    for (unsigned n = 0; n < SMP_ADDS; n++) {
        mutex_lock(&smp_mtx);
        smp_counter++;
        mutex_unlock(&smp_mtx);
    }
}

/*
 * ---- 期限排程的測試 ----
 * 讓每顆 CPU 都被一條一般優先權的執行緒吃滿,然後看一個每 20 毫秒一次的即時工作:
 *   1. 十個週期都準時開始(晚到的 tick 數很小),沒有錯過期限
 *   2. 故意用超過預算:被記一次超出預算,但還來得及在期限前做完
 *   3. 故意做到跨過兩個期限:被記兩次錯過
 *   4. 會讓即時工作合計超過上限的要求被拒絕
 */
static volatile int rt_hog_stop;
static volatile unsigned rt_max_late;

static void rt_hog(void *arg)
{
    (void)arg;
    while (!rt_hog_stop)
        ;
}

static void rt_spin_until(uint64_t tick)
{
    while (timer_ticks() < tick)
        ;
}

static void rt_task(void *arg)
{
    struct rt_stats st;
    unsigned late = 0;

    (void)arg;
    KASSERT(thread_wait_period(&st) == -E_INVAL);        /* 還不是週期性工作 */
    /* 自己原本的優先權設成「高」:超出預算被降級之後仍然排在那些吃 CPU 的執行緒前面,
       測試的時間才是確定的(降級之後要和別人輪流的話,什麼時候做完就看運氣了) */
    thread_set_priority(PRIO_HIGH);
    /* 基準時間要在宣告「之前」讀:宣告之後才讀的話,中間剛好過了一個 tick 就會以為被提早叫醒。
       (因此量到的「晚到」可能比實際多 1 個 tick) */
    uint64_t expect = timer_ticks() + 20;
    KASSERT(thread_set_periodic(20, 5) == 0);
    KASSERT(thread_effective_prio(thread_current()) == PRIO_RT);
    for (int i = 0; i < 10; i++, expect += 20) {
        KASSERT(thread_wait_period(&st) == 0);
        uint64_t now = timer_ticks();
        KASSERT(now >= expect);                          /* 不會提早 */
        if (now - expect > late)
            late = (unsigned)(now - expect);
        rt_spin_until(now + 1);                          /* 做一點點事 */
    }
    KASSERT(st.releases == 10 && st.misses == 0 && st.overruns == 0 && st.period_ms == 20);
    rt_max_late = late;

    /* 超出預算:這個週期用 12 毫秒(預算 5)。仍然在 20 毫秒的期限內做完 */
    rt_spin_until(timer_ticks() + 12);
    KASSERT(thread_effective_prio(thread_current()) == PRIO_HIGH);       /* 已經被降回原本的優先權 */
    KASSERT(thread_wait_period(&st) == 0);
    KASSERT(st.overruns == 1 && st.misses == 0 && st.max_used_ms >= 5 && st.throttled == 0);
    KASSERT(thread_effective_prio(thread_current()) == PRIO_RT);         /* 新的週期:恢復 */

    /* 錯過期限:一口氣做 45 毫秒,跨過兩個期限 */
    rt_spin_until(timer_ticks() + 45);
    KASSERT(thread_wait_period(&st) == 0);
    KASSERT(st.misses == 2 && st.overruns >= 3);

    /* 負載檢查:改成 80% 會被拒絕,而且原本的設定不受影響 */
    KASSERT(thread_set_periodic(10, 8) == -E_BUSY && thread_set_periodic(10, 10) == -E_INVAL);
    KASSERT(thread_wait_period(&st) == 0 && st.period_ms == 20 && st.total_util == 250);
    KASSERT(thread_set_periodic(0, 0) == 0 && thread_wait_period(NULL) == -E_INVAL);
    rt_hog_stop = 1;
}

static void rt_demo(void)
{
    int hogs[MAX_CPUS], n = 0;

    rt_hog_stop = 0;
    for (int i = 0; i < cpu_count && i < MAX_CPUS; i++)
        if ((hogs[n] = thread_create("rt-hog", rt_hog, NULL)) > 0)
            n++;
    int t = thread_create("rt-task", rt_task, NULL);
    KASSERT(t > 0 && n >= 1);
    thread_join(t);
    for (int i = 0; i < n; i++)
        thread_join(hogs[i]);
    kprintf("rt: a 20 ms periodic task ran 10 periods on time with every CPU busy (worst lateness %u "
            "tick%s); a budget overrun and 2 missed deadlines were detected; 80%% load was refused\n",
            rt_max_late, rt_max_late == 1 ? "" : "s");
    KASSERT(rt_max_late <= 3);
}

static void smp_sched_demo(void)
{
    int tid[SMP_WORKERS];
    int ncpu = smp_started();

    if (ncpu == 1) {
        kprintf("smp: only one cpu, scheduling demo skipped\n");
        return;
    }
    uint64_t t0 = timer_ticks();
    unsigned long sw0 = sched_switch_count();
    for (unsigned i = 0; i < SMP_WORKERS; i++)
        tid[i] = thread_create("smp-worker", smp_worker, (void *)(uintptr_t)i);
    for (unsigned i = 0; i < SMP_WORKERS; i++)
        thread_join(tid[i]);

    unsigned all = 0, migrated = 0;
    unsigned long spins = 0;
    for (unsigned i = 0; i < SMP_WORKERS; i++) {
        all |= smp_seen[i];
        spins += smp_spins[i];
        if (smp_seen[i] & (smp_seen[i] - 1))         /* 超過一個位元 = 待過不只一顆 CPU */
            migrated++;
    }
    kprintf("smp: %u busy threads on %d cpus for 30 ms: cpus used mask 0x%x, "
            "%u threads migrated, %lu loop iterations, %lu context switches, %llu ticks\n",
            SMP_WORKERS, ncpu, all, migrated, spins, sched_switch_count() - sw0,
            (unsigned long long)(timer_ticks() - t0));
    kprintf("smp: mutex under real parallelism: counter %lu (expected %lu)\n",
            smp_counter, (unsigned long)SMP_WORKERS * SMP_ADDS);
    KASSERT(all == (1u << ncpu) - 1);                /* 每顆 CPU 都執行過執行緒 */
    KASSERT(smp_counter == (unsigned long)SMP_WORKERS * SMP_ADDS);
}

/*
 * spinlock 與每顆 CPU 狀態的自我測試(目前只有一顆 CPU,檢查的是基本行為與記帳):
 *   - 上鎖後 spin_held 為真、這顆 CPU 的持有數加一;解鎖後恢復
 *   - 巢狀持有兩把「不同的」鎖是允許的
 *   - 解鎖後中斷狀態回到上鎖之前的樣子
 */
static void spin_selftest(void)
{
    static struct spinlock a = SPINLOCK_INIT("test-a"), b = SPINLOCK_INIT("test-b");

    unsigned long fa = spin_lock_irqsave(&a);
    struct cpu *c = this_cpu();              /* 中斷已關閉,可以安全使用 */
    int base = c->nlocks;
    KASSERT(spin_held(&a) && !spin_held(&b) && base >= 1);
    unsigned long fb = spin_lock_irqsave(&b);
    KASSERT(spin_held(&b) && c->nlocks == base + 1);
    spin_unlock_irqrestore(&b, fb);
    KASSERT(!spin_held(&b) && c->nlocks == base);
    int id = c->id;
    spin_unlock_irqrestore(&a, fa);
    KASSERT(!a.locked && a.cpu == -1);

    /* 中斷有恢復:睡一下還醒得來就代表計時器中斷仍在進來 */
    thread_sleep(2);
    kprintf("spinlock: selftest ok (cpu %d of %d online, max %d)\n", id, cpu_count, MAX_CPUS);
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
    KASSERT(b > a);                          /* first-fit:由低位址往高位址配置 */
    KASSERT((uintptr_t)a % PAGE_SIZE == 0);
    pmm_free(a);
    KASSERT(pmm_alloc() == (void *)a);       /* 釋放後最低的空閒頁會被重複使用 */
    char *c = pmm_alloc_pages(8);            /* 連續 8 頁 */
    KASSERT(c > b);                          /* 不與前面配置的頁重疊 */
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

/*
 * 開機標誌。由 kernel 自己印出,所以三個架構完全相同(走的是同一份程式),
 * 序列埠與 i386 的螢幕上都看得到。寬度 55 個字元,80 欄的文字畫面放得下。
 * 不交給各平台的韌體:只有 RISC-V 的韌體會印標誌,而且每個平台的韌體都不一樣。
 */
static const char boot_logo[] =
    "\n"
    " __          __  ______   _            ____     _____\n"
    " \\ \\        / / |  ____| | |          / __ \\   / ____|\n"
    "  \\ \\  /\\  / /  | |__    | |  _   _  | |  | | | (___\n"
    "   \\ \\/  \\/ /   |  __|   | | | | | | | |  | |  \\___ \\\n"
    "    \\  /\\  /    | |____  | | | |_| | | |__| |  ____) |\n"
    "     \\/  \\/     |______| |_|  \\__, |  \\____/  |_____/\n"
    "                               __/ |\n"
    "                              |___/\n";

/* kernel 主函式。主控台此時已可使用。此函式不會返回。 */
void kmain(const struct boot_info *bi)
{
    kprintf("%s\n", boot_logo);
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
    vm_init(bi);                             /* 開啟分頁;之後的存取都經過頁表 */
    /* 開機參數 selftest:執行各子系統的自我測試與示範。預設不執行,
       開機才不會被測試拖慢;回歸測試與 `run` target 會帶上這個參數 */
    int selftest = cmdline_has(bi->cmdline, "selftest");
    if (selftest) {
        vm_demo();
        mem_selftest();
    }
    if (cmdline_has(bi->cmdline, "memtest"))
        mem_fulltest();

    crash_test(bi->cmdline);

    /* 啟動計時器(1000Hz,一個 tick 1ms)與排程器,再開中斷。
       timer_init 只是設定硬體,中斷要到 arch_irq_enable 才會真的進來。
       從開中斷那一刻起,main 也會和其他執行緒一樣被搶佔 */
    timer_init(1000);
    sched_init();
    arch_irq_enable();
    timer_clock_init();                      /* 向硬體的即時時鐘問一次現在的時間 */
    kprintf("timer: %u Hz\n", timer_hz());
    console_init();                          /* 序列埠收到字元時發中斷(中斷控制器已由 timer_init 設定好) */

    /* 檔案系統:建立根目錄,把嵌在 kernel 裡的開機檔案包解開進去。
       要在排程器之後:檔案系統用 mutex 上鎖,而 mutex 需要知道「目前的執行緒」 */
    {
        extern const uint8_t rootfs_start[], rootfs_end[];
        vfs_init();
        int nfiles = initrd_unpack(rootfs_start, (size_t)(rootfs_end - rootfs_start));
        if (nfiles < 0)
            panic("fs: cannot unpack the boot archive (error %d)", nfiles);
        kprintf("fs: root file system ready: %d files unpacked from a %zu KB boot archive\n",
                nfiles, (size_t)(rootfs_end - rootfs_start) / 1024);
        disk_init(bi->cmdline);              /* 有磁碟的話,掛載到 /disk(或決定進入安全模式) */
        if (cmdline_has(bi->cmdline, "crash=boot"))
            panic("test: this boot fails before it completes");      /* 測試安全模式用 */
        if (safe_mode)
            selftest = 0;                    /* 安全模式只做最少的事:不跑自我測試 */
        boot_nic = safe_mode ? NULL : arch_net_probe();   /* 網路卡(安全模式不啟動) */
        if (boot_nic)
            kprintf("net: %s, address %02x:%02x:%02x:%02x:%02x:%02x\n", boot_nic->name,
                    boot_nic->mac[0], boot_nic->mac[1], boot_nic->mac[2], boot_nic->mac[3],
                    boot_nic->mac[4], boot_nic->mac[5]);
        else
            kprintf("net: none\n");
        disk_stage_test(bi->cmdline);
    }

    /* 啟動其他 CPU。selftest 時所有 CPU 一起跑一輪並行測試 */
    smp_init(bi, cmdline_has(bi->cmdline, "selftest"));

    sched_crash_test(bi->cmdline);
    if (selftest) {
        spin_selftest();
        sched_demo();
        sync_demo();
        prio_demo();
    }

    /* 以上的示範檢查的是「單一 CPU 上的排程行為」(誰先執行、誰搶佔誰),
       所以在其他 CPU 加入之前跑。從這裡起,執行緒會真正同時在多顆 CPU 上執行 */
    if (!safe_mode)
        smp_sched_start();                   /* 安全模式:其他 CPU 維持待命,只用開機的這一顆 */
    if (cmdline_has(bi->cmdline, "crash=wdog")) {
        if (smp_started() < 2) {
            /* 只有一顆 CPU 時沒有人可以看它(見 watchdog.c 的限制):這個測試無法進行 */
            panic("watchdog test needs at least 2 cpus");
        }
        watchdog_set_timeout(300);           /* 測試用較短的期限 */
        kprintf("one cpu now spins with interrupts disabled; the watchdog should notice...\n");
        thread_create("stuck", spin_irqs_off, NULL);
        thread_sleep(3000);
        kprintf("the watchdog did not fire\n");
        arch_exit(0);                        /* 預期不會到這裡:以「成功」結束會讓測試失敗 */
    }
    if (cmdline_has(bi->cmdline, "crash=smp")) {
        /* 其他 CPU 正忙的時候 panic:它們應該被叫停,系統乾淨地結束 */
        for (int i = 0; i < smp_started(); i++)
            thread_create("spinner", spin_forever, NULL);
        thread_sleep(5);
        panic("test panic while %d cpus are busy", smp_started());
    }
    if (selftest) {
        smp_sched_demo();
        rt_demo();
        fs_demo();
        disk_demo();
        nic_selftest();                      /* 驅動程式單獨的測試:要在協定堆疊開始收訊框之前 */
    }
    netif_start(boot_nic);                   /* 啟動網路(沒有網路卡時不做任何事) */
    if (selftest) {
        pipe_demo();
        user_demo();
    }
    if (cmdline_has(bi->cmdline, "stress"))
        sched_stress();

    kprintf("uptime: %llu ticks, %lu context switches\n",
            (unsigned long long)timer_ticks(), sched_switch_count());

    /*
     * 啟動 shell:從主控台讀命令並執行。
     *   一般開機(沒有 autoexit):一定啟動,這是使用者與系統互動的地方;
     *                              shell 結束(exit 或在空的提示符號下按 Ctrl-D)就關機
     *   自動化測試(autoexit)    :只有另外給了 shell 參數才啟動;shell 結束後
     *                              以它的結束碼結束模擬器
     */
    int exit_code = 0;
    /* 測試用:確認這次開機是不是安全模式 */
    if (cmdline_has(bi->cmdline, "expect=safe"))
        KASSERT(safe_mode && boot_nic == NULL && !scrub_running && cpu_count == 1);
    if (cmdline_has(bi->cmdline, "expect=normal"))
        KASSERT(!safe_mode);
    /* 走到這裡表示開機順利完成:記下來。之後即使被直接關掉電源,也不算「開機失敗」。
       安全模式不在這裡記:要等有人看過、正常離開之後(見 shutdown) */
    if (!safe_mode)
        blackbox_log(BB_HEALTHY, "boot completed");
    int shell = !autoexit || cmdline_has(bi->cmdline, "shell");
    if (shell) {
        kprintf("starting /bin/sh (type \"help\" for a list of commands)\n");
        int pid = proc_spawn("/bin/sh", NULL);
        if (pid < 0)
            panic("cannot start /bin/sh");
        exit_code = proc_wait(pid);
        kprintf("shell exited with code %d\n", exit_code);
    }
    if (autoexit)
        shutdown(exit_code);
    if (shell) {
        /* 互動使用時 shell 結束了:沒有東西可以再接受命令,關機(結束模擬器)。
           結束碼固定為 0:這是正常的離開,不是測試失敗 */
        kprintf("powering off\n");
        shutdown(0);
    }
    for (;;)
        thread_sleep(1000);                  /* main 無事可做;CPU 交給 idle 執行緒休眠 */
}
