/*
 * sched_test.c - 排程器、同步機制、優先權、多核心、期限排程的示範與測試,以及壓力與當機測試
 *
 * 這些程式原本和開機流程一起放在 kernel/main.c(第 43 步拆出)。它們只在開機參數
 * 要求時執行(selftest、crash=、disktest=、stress、memtest),對外的進入點宣告在 include/selftest.h。
 */
#include <stdint.h>
#include "clksync.h"
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
#include "selftest.h"
#include "uapi/selftest.h"

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

/* 每條執行緒記錄兩個數字:
     iters  迴圈跑了幾圈(看得出它有在執行)
     ticks  排程器記在它頭上的 CPU 時間(tick 數;和時間片用的是同一份記帳)
   公平性看的是 ticks。iters 不能拿來比:同樣的時間能跑幾圈,在模擬器上會受
   「這條執行緒的 stack 剛好配置在哪一頁」影響到好幾倍(見 DEVLOG 第 21 步) */
struct spin_stat {
    volatile unsigned long iters;
    volatile unsigned long ticks;
};

/*
 * 兩條忙碌的執行緒一起跑到「兩者合計用掉 spin_budget 個 tick 的 CPU 時間」為止。
 * 以 CPU 時間、而不是牆上時間決定長度:模擬器停頓時,一段固定的牆上時間裡可能只容得下
 * 三個多時間片,這時誰多拿一片純屬切在哪裡(實際發生過:27 對 10)。
 */
static struct spin_stat *spin_pair[2];
static volatile unsigned long spin_budget;

static void spin_worker(void *arg)
{
    struct spin_stat *st = arg;
    unsigned long t0 = thread_cpu_ticks();

    while (spin_pair[0]->ticks + spin_pair[1]->ticks < spin_budget) {
        st->iters++;
        st->ticks = thread_cpu_ticks() - t0;
    }
}

/* 示範 3:休眠。各自睡不同長度,醒來的順序應該依時間而不是建立順序 */
static void sleep_worker(void *arg)
{
    unsigned ms = (unsigned)(uintptr_t)arg;
    thread_sleep(ms);
    kprintf("  [%s] woke after %u ms (tick %llu)\n", thread_name(), ms,
            (unsigned long long)timer_ticks());
}

void sched_demo(void)
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
    struct timer_stats tsa, tsb;
    timer_get_stats(&tsa);
    unsigned slice = timer_hz() * 10 / 1000;                  /* 時間片:10 毫秒(見 sched.c 的 TIME_SLICE_MS) */
    s1.ticks = s2.ticks = s1.iters = s2.iters = 0;
    spin_pair[0] = &s1;
    spin_pair[1] = &s2;
    spin_budget = 6 * slice;                                   /* 合計 6 個時間片的 CPU 時間 */
    /* 兩條要同時起跑:建立期間 main 用較高的優先權,時間片在兩次建立之間用完也不會讓 spin1 先跑。
       第 51 步在 aarch64 量到 40 對 21:main 建立 spin1 後時間片剛好用完,spin1 先跑了一整片,
       main 回來建立 spin2 之後 spin1 又排在前面 — 不是排程器的錯,是測試的起跑不公平。
       在兩次建立之間故意忙等 12 毫秒可以必定重現(修正前 40 對 21,修正後 31 對 30) */
    thread_set_priority(PRIO_HIGH);
    t[0] = thread_create("spin1", spin_worker, &s1);
    t[1] = thread_create("spin2", spin_worker, &s2);
    join_all(t, 2);
    thread_set_priority(PRIO_NORMAL);
    timer_get_stats(&tsb);
    kprintf("  spin1: %lu ms of cpu (%lu loops), spin2: %lu ms (%lu loops), %lu context switches; "
            "late timer interrupts in the window: %u (3+ ticks: %u, largest ever %u)\n",
            s1.ticks, s1.iters, s2.ticks, s2.iters, sched_switch_count() - sw0,
            timer_bursts_at_least(&tsb, 2) - timer_bursts_at_least(&tsa, 2),
            timer_bursts_at_least(&tsb, 3) - timer_bursts_at_least(&tsa, 3), tsb.max_burst);
    /* 輪流的保證:兩條同優先權的執行緒,任何時候拿到的 CPU 時間相差不超過一個時間片
       (加 1:一條看到總和到了才停,另一條可能多記了最後一個 tick) */
    unsigned long c1 = s1.ticks, c2 = s2.ticks, diff = c1 > c2 ? c1 - c2 : c2 - c1;
    if (!s1.iters || !s2.iters || !c1 || !c2 || diff > slice + 1) {
        kprintf("sched fairness failed; spin1 is thread %d, spin2 is thread %d\n", t[0], t[1]);
        (void)sched_trace_dump_all(40);
        panic("sched fairness: cpu ticks %lu vs %lu (difference %lu, allowed %u = one time slice + 1)",
              c1, c2, diff, slice + 1);
    }

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
static struct mutex race_mutex = MUTEX_INIT("test-race", MX_TEST);

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
static struct mutex pc_mutex = MUTEX_INIT("test-pc", MX_TEST);
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

void sync_demo(void)
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
/*
 * 搶佔延遲:睡眠的一方醒來之後,要等多久才真的開始執行(排程延遲)。
 *   比忙碌的一方優先:醒來就搶佔,延遲只來自計時器的抖動
 *   同優先權:要等忙碌的一方用完它的時間片,延遲至少是時間片剩下的部分
 *
 * 量的是「延遲」本身,不是「總共過了多久」,而且不依賴任何固定的牆上時間:
 *   - 忙碌的一方一直執行到睡眠的一方量完為止(不是固定空轉幾毫秒 — 模擬器停頓時,
 *     一個時間片在牆上時間可以拉得很長,固定的長度會讓忙碌的一方提早結束,兩組量到同樣的結果)
 *   - 睡眠的一方要睡之前發出信號,忙碌的一方立刻讓出一次、拿到一個全新的時間片(10 個 tick)。
 *     睡眠是 6 個 tick,所以醒來時那個時間片至少還剩 2 個 tick 以上(補記的 tick 不扣時間片)
 */
static volatile int lat_sleeping, lat_done;
static volatile unsigned lat_delay;
static volatile uint64_t lat_t1;

static void latency_sleeper(void *arg)
{
    (void)arg;
    lat_sleeping = 1;
    while (lat_sleeping == 1)                /* 等忙碌的一方確認看到了(它會改成 2 並讓出一次) */
        thread_yield();
    uint64_t t1 = timer_ticks();
    lat_t1 = t1;
    thread_sleep(5);                         /* 5 毫秒 = 6 個 tick(見 thread_sleep 的換算) */
    uint64_t t2 = timer_ticks();
    lat_delay = t2 > t1 + 6 ? (unsigned)(t2 - t1 - 6) : 0;
    lat_done = 1;
}

static void latency_spinner(void *arg)
{
    (void)arg;
    while (lat_sleeping != 1)
        ;
    lat_sleeping = 2;
    thread_yield();                          /* 讓對方去睡;回來時從一個全新的時間片開始 */
    while (!lat_done)
        ;
}

/* 其他 CPU 上的高優先權忙碌執行緒:「同優先權必須等對方的時間片」的前提是,剛醒的睡眠者只能和
   spinner 搶同一顆 CPU。有閒著的 CPU 時,工作竊取會立刻把它拿去跑(延遲 1 tick,是更好的行為);
   任一 CPU 都能叫醒睡眠者(I-337)之後這種情形變多,i386 6 次有 1 次量到 1 而誤判。
   第一版用「同優先權」的忙碌執行緒佔住其他 CPU 仍不對:睡眠者可能落在某顆忙碌執行緒時間片快用完的
   CPU 上,延遲 1 是合理的(riscv64 出現一次)。改成高優先權:它們不讓出、也不竊取比自己低的工作,
   睡眠者只能等 spinner — spinner 在睡眠者入睡時剛拿到全新的時間片 */
static volatile int lat_hog_stop, lat_hog_running;

static void lat_hog(void *arg)
{
    (void)arg;
    __atomic_fetch_add(&lat_hog_running, 1, __ATOMIC_SEQ_CST);
    while (!lat_hog_stop)
        ;
}

/* 回傳排程延遲(tick);*jitter 得到這段期間計時器一次補記最多的 tick 數(依分組的下限,保守) */
static unsigned latency_run(int sleeper_prio, unsigned *jitter)
{
    int t[2], hogs[MAX_CPUS];
    unsigned nh = 0;
    struct timer_stats a, b;

    lat_sleeping = lat_done = 0;
    lat_hog_stop = 0;
    lat_hog_running = 0;
    if (sleeper_prio == PRIO_NORMAL)             /* 同優先權那一輪:其他 CPU 都佔住 */
        for (int i = 1; i < cpu_count && nh < MAX_CPUS; i++) {
            int h = thread_create_prio("lat-hog", lat_hog, NULL, PRIO_HIGH);
            KASSERT(h > 0);
            hogs[nh++] = h;
        }
    /* 等它們都真的在跑(各佔一顆 CPU)才開始:剛建立時可能還沒排上,短時間內仍有閒著的 CPU
       會把睡眠者拿走(高優先權版本 12 次仍有 1 次量到 1) */
    for (int w = 0; __atomic_load_n(&lat_hog_running, __ATOMIC_SEQ_CST) < (int)nh; w++) {
        KASSERT(w < 5000);
        thread_sleep(1);
    }
    timer_get_stats(&a);
    t[0] = thread_create_prio("spinner", latency_spinner, NULL, PRIO_NORMAL);
    t[1] = thread_create_prio("sleeper", latency_sleeper, NULL, sleeper_prio);
    join_all(t, 2);
    timer_get_stats(&b);
    lat_hog_stop = 1;
    for (unsigned i = 0; i < nh; i++)
        thread_join(hogs[i]);
    static const unsigned lo[TIMER_BURST_BUCKETS] = { 2, 3, 4, 6, 7, 10 };
    *jitter = 1;
    for (unsigned k = 0; k < TIMER_BURST_BUCKETS; k++)
        if (b.burst[k] != a.burst[k])
            *jitter = k == TIMER_BURST_BUCKETS - 1 ? b.max_burst : lo[k + 1 < TIMER_BURST_BUCKETS ? k + 1 : k] - 1;
    return lat_delay;
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

    mutex_init(&inv_mutex, "test-inversion", MX_TEST);
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

void prio_demo(void)
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

    unsigned jit_same, jit_high;
    unsigned same = latency_run(PRIO_NORMAL, &jit_same);
    unsigned high = latency_run(PRIO_HIGH, &jit_high);
    kprintf("prio: sleep(5 ms) while another thread spins: scheduling delay after the wake-up time: "
            "same priority %u ticks, high priority %u (timer jitter in that window: %u)\n", same, high, jit_high);
    /* 高優先權:醒來就搶佔 — 延遲只能來自計時器的抖動(量到的),再加讀取時間的 1 個 tick。
       同優先權:必須等對方時間片剩下的部分,至少 2 個 tick */
    if (high > jit_high + 1 || same < 2) {
        (void)sched_trace_dump_all(40);
        panic("prio latency: high-priority delay %u (allowed %u), same-priority delay %u (must be >= 2)",
              high, jit_high + 1, same);
    }

    unsigned without = inversion_run(0);
    unsigned with = inversion_run(1);
    kprintf("prio: inversion: high waited %u ticks for the lock without inheritance, "
            "%u with\n", without, with);
    KASSERT(with < without && with <= 10 && without >= 15);

    /* 完整的繼承。此時 main 是最高優先權,各執行緒靠 main 睡覺的空檔依序就位 */
    mutex_init(&pi_a, "test-pi-a", MX_TEST_PI_A);
    mutex_init(&pi_b, "test-pi-b", MX_TEST_PI_B);
    mutex_init(&pi_c, "test-pi-c", MX_TEST_PI_C);
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

void sched_stress(void)
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
void spin_irqs_off(void *arg)
{
    (void)arg;
    arch_irq_disable();
    for (;;)
        ;
}

/* crash=smp 用:永遠忙碌的執行緒 */
void spin_forever(void *arg)
{
    (void)arg;
    for (;;)
        ;
}

void sched_crash_test(const char *cmd)
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
        struct vm_space *vs = vm_space_create(NULL);
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
        static struct mutex m = MUTEX_INIT("test-relock", MX_TEST);
        kprintf("locking a mutex twice...\n");
        mutex_lock(&m);
        mutex_lock(&m);
    } else if (cmdline_has(cmd, "crash=spinsleep")) {
        /* 持有 spinlock 時切換執行緒:別人會關著中斷一直等這把鎖,所以禁止 */
        static struct spinlock l = SPINLOCK_INIT("test", LK_TEST_OUTER);
        kprintf("yielding the CPU while holding a spinlock...\n");
        unsigned long flags = spin_lock_irqsave(&l);
        thread_yield();
        spin_unlock_irqrestore(&l, flags);
    } else if (cmdline_has(cmd, "crash=mxorder")) {
        /* mutex 以相反的順序上鎖(內層的先拿):mutex 的層級檢查必須立刻 panic(見 sync.h) */
        static struct mutex outer = MUTEX_INIT("test-mx-outer", MX_TEST_PI_B);
        static struct mutex inner = MUTEX_INIT("test-mx-inner", MX_TEST_PI_A);
        kprintf("locking mutexes in the wrong order...\n");
        mutex_lock(&inner);
        mutex_lock(&outer);
        mutex_unlock(&outer);
        mutex_unlock(&inner);
    } else if (cmdline_has(cmd, "crash=lockorder")) {
        /* 以相反的順序拿兩把鎖(內層的先拿):鎖的層級檢查必須立刻 panic(見 spinlock.h)。
           不檢查的話,單獨一條路徑這樣做不會出事,要等另一條路徑以正常順序同時進行才會卡死 */
        static struct spinlock outer = SPINLOCK_INIT("test-outer", LK_TEST_OUTER);
        static struct spinlock inner = SPINLOCK_INIT("test-inner", LK_TEST_INNER);
        kprintf("acquiring spinlocks in the wrong order...\n");
        unsigned long f1 = spin_lock_irqsave(&inner);
        unsigned long f2 = spin_lock_irqsave(&outer);
        spin_unlock_irqrestore(&outer, f2);
        spin_unlock_irqrestore(&inner, f1);
    } else if (cmdline_has(cmd, "crash=spin")) {
        /* 同一顆 CPU 對同一把 spinlock 上鎖兩次:不偵測的話會自己等自己,而且中斷是關的 */
        static struct spinlock l = SPINLOCK_INIT("test", LK_TEST_OUTER);
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
static struct mutex smp_mtx = MUTEX_INIT("test-smp", MX_TEST);
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
    /* 第一個週期以真實時間為起點(sched.c 用 timer_ticks_real,見 timer.h):預期值也要用同一個基準,
       否則第 0 顆 CPU 的 tick 落後時,落後的量會被誤算成「晚到」 */
    uint64_t expect = timer_ticks_real() + 20;
    KASSERT(thread_set_periodic(20, 5) == 0);
    KASSERT(thread_effective_prio(thread_current()) == PRIO_RT);
    for (int i = 0; i < 10; i++, expect += 20) {
        KASSERT(thread_wait_period(&st) == 0);
        /* 醒來的時間也用 timer_ticks_real():釋放的判斷與 expect 都以它為準。以前讀 timer_ticks()
           (CPU0 累計、會落後),CPU0 的 tick 落後時讀到的「現在」比 expect 小,看起來像提早醒來
           (第 65 步完整回歸 i386 8 顆 CPU 出現一次 now >= expect 失敗;根源是 I-337 的兩套時間) */
        uint64_t now = timer_ticks_real();
        KASSERT(now >= expect);                          /* 不會提早 */
        if (now - expect > late)
            late = (unsigned)(now - expect);
        rt_spin_until(now + 1);                          /* 做一點點事 */
    }
    if (!(st.releases == 10 && st.misses == 0 && st.overruns == 0 && st.period_ms == 20))
        kprintf("rt: periodic stats mismatch: releases %u, misses %u, overruns %u, period %u ms, "
                "max late %u ticks\n", (unsigned)st.releases, (unsigned)st.misses,
                (unsigned)st.overruns, (unsigned)st.period_ms, late);   /* 失敗時留下是哪一項 */
    KASSERT(st.releases == 10 && st.misses == 0 && st.overruns == 0 && st.period_ms == 20);
    rt_max_late = late;

    /*
     * 超出預算:這個週期用 12 毫秒(預算 5)。仍然在 20 毫秒的期限內做完。
     *
     * 嚴格的檢查(降級了、超出預算正好 1 次、沒有錯過期限)預設一定要成立。唯一的例外要「量得到」:
     * 空轉從週期開始後約 2 個 tick 開始、做 12 個,離下一個週期還有約 6 個 tick 的餘裕。
     * 只有計時器一次跳過 >= 6 個 tick(虛擬 CPU 被停住,timer_max_burst_since 量到的)才可能跨進
     * 下一個週期。一次補記 2~3 個是常態(中斷晚到 1~2 毫秒),那不構成例外。
     * 沒有量到夠大的跳躍卻不成立,就是排程器的錯:panic 並印出所有數值。
     */
    struct timer_stats ts0, ts1;
    timer_get_stats(&ts0);
    uint64_t spin_from = timer_ticks();
    unsigned long boosts0 = sched_starve_boosts(), sw0 = sched_switch_count();
    unsigned long cpu0 = thread_cpu_ticks();
    rt_spin_until(spin_from + 12);
    unsigned long boosts1 = sched_starve_boosts(), sw1 = sched_switch_count();
    unsigned long cpu1 = thread_cpu_ticks();
    uint64_t spin_end = timer_ticks();
    int prio_after = thread_effective_prio(thread_current());
    timer_get_stats(&ts1);
    unsigned jump = timer_bursts_at_least(&ts1, 6) - timer_bursts_at_least(&ts0, 6);   /* 這段期間跳過 >= 6 的次數 */
    int stalled = jump > 0;
    KASSERT(thread_wait_period(&st) == 0);
    /* 預算以「記到的 CPU 時間」計算;中斷晚到後一次補記的 tick 不算進任何執行緒(第 48 步的設計,
       docs/ISSUES.md I-283)。所以判斷要看實際記到多少:空轉期間沒有切換、卻只記到 <= 5 個 tick 時,
       依這個記帳方式本來就沒有超出預算 — 印出量到的值,不當成排程器的錯 */
    unsigned long charged = cpu1 - cpu0;
    int under_charged = sw1 == sw0 && charged <= 5;
    if (under_charged)
        kprintf("rt: the 12-tick spin was charged only %lu ticks (late timer interrupts are not charged); "
                "overrun checks skipped\n", charged);
    if (!stalled && !under_charged &&
        (prio_after != PRIO_HIGH || st.overruns != 1 || st.misses != 0 || st.max_used_ms < 5)) {
        (void)sched_trace_dump_all(40);
        panic("rt test (overrun): prio %d (want %d), overruns %u, misses %u, max used %u ms; "
              "spin %llu..%llu (ended %llu), deadline %llu; my cpu ticks %lu, switches %lu, "
              "starvation boosts %lu, timer jumps 6+: %u",
              prio_after, PRIO_HIGH, st.overruns, st.misses, st.max_used_ms,
              (unsigned long long)spin_from, (unsigned long long)(spin_from + 12),
              (unsigned long long)spin_end, (unsigned long long)expect, cpu1 - cpu0, sw1 - sw0,
              boosts1 - boosts0, jump);
    }
    if (stalled) {
        kprintf("rt: measured %u timer jump(s) of 6+ ticks during the overrun test (its slack is 6 ticks); "
                "its same-period checks were skipped\n", jump);
    }
    KASSERT((st.overruns >= 1 || under_charged) && st.throttled == 0);
    KASSERT(thread_effective_prio(thread_current()) == PRIO_RT);         /* 新的週期:恢復 */

    /* 錯過期限:一口氣做 45 毫秒,跨過兩個期限(主機停頓時可能更多,所以只要求「至少」) */
    unsigned misses0 = st.misses, overruns0 = st.overruns;
    rt_spin_until(timer_ticks() + 45);
    KASSERT(thread_wait_period(&st) == 0);
    if (st.misses - misses0 < 2 || st.overruns - overruns0 < 2)
        panic("rt test (missed deadlines): misses %u -> %u, overruns %u -> %u", misses0, st.misses,
              overruns0, st.overruns);

    /* 負載檢查:改成 80% 會被拒絕,而且原本的設定不受影響 */
    KASSERT(thread_set_periodic(10, 8) == -E_BUSY && thread_set_periodic(10, 10) == -E_INVAL);
    KASSERT(thread_wait_period(&st) == 0 && st.period_ms == 20 && st.total_util == 250);
    KASSERT(thread_set_periodic(0, 0) == 0 && thread_wait_period(NULL) == -E_INVAL);
    rt_hog_stop = 1;
}

/*
 * 長週期與短週期的即時工作同時存在(Codex 找到的反例):長的那個每 100 毫秒忙 25 毫秒,
 * 短的那個每 10 毫秒要醒來一次。短的必須能打斷長的 — 只有一顆 CPU 時尤其如此:
 * 兩者在同一層輪流的話,長的會用完它 10 毫秒的時間片,短的每個忙碌區段要錯過兩三次。
 * 容許錯過 1 次:在模擬器上,主機偶爾停頓十幾毫秒,那不是排程器的問題。
 */
static volatile int rt_long_ready, rt_long_stop;
static volatile unsigned rt_long_misses, rt_short_misses;

static void rt_long_task(void *arg)
{
    struct rt_stats st;

    (void)arg;
    KASSERT(thread_set_periodic(100, 30) == 0);
    rt_long_ready = 1;
    while (!rt_long_stop) {
        rt_spin_until(timer_ticks() + 25);
        KASSERT(thread_wait_period(&st) == 0);
    }
    rt_long_misses = st.misses;
    KASSERT(thread_set_periodic(0, 0) == 0);
}

static void rt_short_task(void *arg)
{
    struct rt_stats st;

    (void)arg;
    while (!rt_long_ready)
        thread_yield();
    struct timer_stats ts0, ts1;
    timer_get_stats(&ts0);
    KASSERT(thread_set_periodic(10, 3) == 0);
    /* 週期短的排在比較高的層級 */
    KASSERT(thread_effective_prio(thread_current()) == PRIO_RT + 1);
    for (int i = 0; i < 20; i++)                         /* 200 毫秒:橫跨長工作的兩個忙碌區段 */
        KASSERT(thread_wait_period(&st) == 0);
    rt_short_misses = st.misses;
    /* 不可以錯過任何期限。例外要量得到:週期 10、預算 3,一次跳過 >= 7 個 tick 才吃得掉餘裕 */
    timer_get_stats(&ts1);
    unsigned jump = timer_bursts_at_least(&ts1, 7) - timer_bursts_at_least(&ts0, 7);
    if (st.releases + st.misses < 20 || st.misses > jump) {
        (void)sched_trace_dump_all(40);
        panic("rt test (rate-monotonic): releases %u, misses %u, timer jumps of 7+ ticks in the window: %u",
              st.releases, st.misses, jump);
    }
    rt_long_stop = 1;
    KASSERT(thread_set_periodic(0, 0) == 0);
}

void rt_demo(void)
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

    rt_long_ready = rt_long_stop = 0;
    int tl = thread_create("rt-long", rt_long_task, NULL), ts = thread_create("rt-short", rt_short_task, NULL);
    KASSERT(tl > 0 && ts > 0);
    thread_join(ts);
    thread_join(tl);
    KASSERT(rt_long_misses == 0);
    kprintf("rt: a 10 ms task missed %u of 20 deadlines while a 100 ms task was busy 25 ms per period "
            "(shorter period = higher level)\n", rt_short_misses);
}

void smp_sched_demo(void)
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
    /* 排程紀錄必須涵蓋每一顆 CPU:上面已證明每顆都執行過執行緒(因此都有切換紀錄),
       所以「有紀錄而印出來的 CPU 數」必須等於 CPU 數。只印目前這顆的舊版會在這裡得到 1 */
    int traced = sched_trace_dump_all(1);
    kprintf("smp: scheduler trace covers %d of %d cpus\n", traced, ncpu);
    KASSERT(traced == ncpu);
}

/*
 * ---- 睡眠不可以提早醒 ----
 * thread_sleep(ms) 承諾「至少睡 ms 毫秒」。系統時間(tick)只由第 0 顆 CPU 的計時器中斷推進,
 * 那顆 CPU 的中斷晚到時(關中斷、模擬器的主機排程),tick 會暫時落後真實時間;
 * 若以落後的 tick 為起點算醒來的時間,醒來時真實經過的時間就會不足(第 51 步在 riscv64、4 顆 CPU
 * 上量到 sleep(100) 只睡了 99.5 毫秒)。
 * 這裡讓每顆 CPU 都忙著,以高精度時鐘(與 tick 無關的硬體計數器)量 300 次 sleep(1),
 * 每一次都必須 >= 1 毫秒。比較用乘法:elapsed * 1000 >= ms * hz,不需要 64 位元除法。
 */
static volatile int sw_stop;

static void sw_hog(void *arg)
{
    (void)arg;
    while (!sw_stop)
        ;
}

/*
 * 必定觸發的情境:在第 0 顆 CPU(唯一推進系統時間的那顆)關中斷 5 毫秒,讓 tick 確實落後,
 * 在落後的狀態下要一個「至少 1 毫秒」的期限,開中斷後量到期時真實經過了多久。
 * 以落後的 tick 為起點(修正前的做法),開中斷時一次補記 5 個 tick,期限立刻就到 — 幾乎 0 毫秒。
 * 回傳經過的時鐘計數。
 */
static uint64_t tick_lag_probe(uint64_t hz)
{
    for (int i = 0; i < 5000 && sched_cpu_id() != 0; i++)
        thread_sleep(1);                     /* 換到第 0 顆 CPU 上(單 CPU 時本來就是) */
    unsigned long fl = arch_irq_save();
    if (sched_cpu_id() != 0) {
        arch_irq_restore(fl);
        panic("sleep test: could not get onto cpu 0");
    }
    uint64_t t0 = timer_clock_count();
    while ((timer_clock_count() - t0) * 1000 < hz * 5)
        ;                                    /* 關著中斷 5 ms:這段期間 tick 不會前進 */
    uint64_t dl = timer_deadline_ms(1), c0 = timer_clock_count();
    arch_irq_restore(fl);
    while (!timer_expired(dl))
        ;
    return timer_clock_count() - c0;
}

void sleep_wall_test(void)
{
    uint64_t hz = timer_clock_hz();
    if (hz == 0) {
        kprintf("sleep: no high-resolution clock, wall-time sleep test skipped\n");
        return;
    }
    uint64_t lag = tick_lag_probe(hz);
    kprintf("sleep: a 1 ms deadline taken while cpu 0 had 5 ms of ticks pending expired after %llu "
            "clock counts (clock %llu Hz)\n", (unsigned long long)lag, (unsigned long long)hz);
    if (lag * 1000 < hz)
        panic("timer deadline taken during a tick lag expired early (%llu counts, clock %llu Hz)",
              (unsigned long long)lag, (unsigned long long)hz);
    int hogs[MAX_CPUS], n = 0;
    sw_stop = 0;
    for (int i = 0; i < cpu_count && i < MAX_CPUS; i++)
        if ((hogs[n] = thread_create("sw-hog", sw_hog, NULL)) > 0)
            n++;
    thread_set_priority(PRIO_HIGH);          /* 醒來就能上 CPU:量到的是「睡了多久」而不是「等了多久才輪到」 */
    uint64_t min = ~0ULL;
    unsigned short_count = 0;
    for (int i = 0; i < 300; i++) {
        uint64_t t0 = timer_clock_count();
        thread_sleep(1);
        uint64_t d = timer_clock_count() - t0;
        if (d < min)
            min = d;
        if (d * 1000 < hz)
            short_count++;
    }
    thread_set_priority(PRIO_NORMAL);
    sw_stop = 1;
    for (int i = 0; i < n; i++)
        thread_join(hogs[i]);
    kprintf("sleep: 300 x sleep(1 ms) with every cpu busy: shortest %llu clock counts "
            "(clock %llu Hz), woke early %u times\n",
            (unsigned long long)min, (unsigned long long)hz, short_count);
    if (short_count) {
        (void)sched_trace_dump_all(20);
        panic("sleep woke early %u of 300 times (shortest %llu counts, clock %llu Hz)",
              short_count, (unsigned long long)min, (unsigned long long)hz);
    }
}

/*
 * ---- 第 0 顆 CPU 卡住時,別的 CPU 照樣推進時間、叫醒休眠者(I-337)----
 * 第 0 顆 CPU 關中斷空轉 CS_STALL_MS 毫秒(用高精度時鐘量,不靠 tick)。同時另一條執行緒在別的 CPU 上
 * 反覆 thread_sleep(CS_SLEEP_MS),每次以高精度時鐘量真正睡了多久。
 *   修正前:系統時間只由第 0 顆推進,這段期間 tick 不動,休眠者要等到第 0 顆開中斷才醒(約 300 ms)
 *           — 這就是負對照:把這個測試套在修正前的 kernel 上,應該在「tick 沒有前進」或「睡太久」失敗。
 *   修正後:tick 照常前進,每次睡眠在 CS_SLEEP_MS + 幾個 tick 之內醒來。
 * 內建對照:第 0 顆的心跳(每次計時器中斷加一)在空轉期間必須完全不變 — 證明它真的沒有處理任何中斷,
 * 而不是測試本身沒把它卡住;推進時間的次數必須記在別顆 CPU 頭上。
 */
#define CS_STALL_MS   300
#define CS_SLEEP_MS   10
#define CS_STRICT_MS  (CS_SLEEP_MS + 4)  /* 設計上限:sleep 的 n = ms + 1 個 tick,加推進端 1 個 tick、
                                            插入競賽 1 個 tick、喚醒通知 1 個 tick(1 tick = 1 ms) */
#define CS_HARD_MS    100                /* 絕對上限:遠小於修正前的 ~300 ms,也容得下模擬器的主機停頓 */
#define CS_MAX_SAMPLES 64
static volatile int cs_phase;            /* 0 = 準備、1 = 第 0 顆關中斷中、2 = 結束 */
static volatile unsigned cs_n, cs_on_cpu0;
static uint64_t cs_t0[CS_MAX_SAMPLES], cs_d[CS_MAX_SAMPLES];   /* 每次睡眠的起點與長度(時鐘計數) */
static uint64_t cs_hz;

static void *volatile cs_sleeper_self;           /* 睡眠者自己(卡住的處理函式用來判斷被打斷的是不是它) */
static volatile int cs_sleeper_cpu = -1;         /* 睡眠者忙等時所在的 CPU */

static void cs_sleeper(void *arg)
{
    (void)arg;
    cs_sleeper_self = thread_current();
    /* 忙等:卡住期間不能靠 sleep 開始(那正是待測項目)。落在第 0 顆時睡 1 毫秒讓出,醒來時有機會換到
       別顆 CPU — 沒有「固定在某顆 CPU」的機制,高優先權又一直忙等的它不會自己離開第 0 顆 */
    while (cs_phase == 0) {
        int c = sched_cpu_id();
        cs_sleeper_cpu = c;
        if (c == 0)
            thread_sleep(1);
    }
    while (cs_phase == 1) {
        uint64_t t0 = timer_clock_count();
        thread_sleep(CS_SLEEP_MS);
        uint64_t d = timer_clock_count() - t0;
        if (cs_phase == 1 && sched_cpu_id() == 0)
            cs_on_cpu0++;                    /* 不可能:第 0 顆關著中斷 */
        if (cs_n < CS_MAX_SAMPLES) {
            cs_t0[cs_n] = t0;
            cs_d[cs_n] = d;
            cs_n++;
        }
    }
}

/* 在第 0 顆 CPU 上、中斷關閉中執行:空轉 CS_STALL_MS 毫秒,前後記下心跳、tick 與時鐘 */
struct cs_stall_ctx {
    int ran_on_cpu0, aborted;
    unsigned long hb0, hb1;
    uint64_t tk0, tk1, c0, c1;
    struct timer_stats st1;
};

static void cs_stall_fn(void *arg)
{
    struct cs_stall_ctx *x = arg;
    x->ran_on_cpu0 = sched_cpu_id() == 0;
    /* 被打斷的正是睡眠者(它在卡住開始前忙等,可能剛好在第 0 顆上):它會跟著卡住、無法被搬走,
       這一輪不算 — 放棄,讓測試稍後重試(18 次曾有 1 次因此一次都沒醒) */
    if (this_cpu()->current == cs_sleeper_self) {
        x->aborted = 1;
        return;
    }
    x->hb0 = this_cpu()->heartbeat;
    x->tk0 = timer_ticks();
    x->c0 = timer_clock_count();
    cs_phase = 1;
    while ((timer_clock_count() - x->c0) * 1000 < cs_hz * CS_STALL_MS)
        ;                                    /* 關中斷空轉:第 0 顆不處理計時器中斷、不推進時間 */
    x->tk1 = timer_ticks();
    x->hb1 = this_cpu()->heartbeat;
    x->c1 = timer_clock_count();
    timer_get_stats(&x->st1);                /* 還在關中斷中取:不含第 0 顆開中斷後自己的補記 */
    /* 卡住結束:在第 0 顆恢復中斷「之前」就換階段。以前由發起者在跨 CPU 呼叫返回後才設,
       中間第 0 顆已經開中斷、睡眠者可能被排上去,階段卻還是 1 → 被誤算成「卡住時跑在第 0 顆」
       (第 68 步完整回歸 aarch64 兩次) */
    cs_phase = 2;
}

void cpu0_stall_wake_test(void)
{
    cs_hz = timer_clock_hz();
    if (cpu_count < 3 || cs_hz == 0) {
        kprintf("cpu0-stall: needs >= 3 cpus and a shared clock, skipped\n");
        return;
    }
    cs_phase = 0;
    cs_n = cs_on_cpu0 = 0;
    cs_sleeper_cpu = -1;
    cs_sleeper_self = NULL;
    uint32_t adv0[MAX_CPUS];
    for (int i = 0; i < MAX_CPUS; i++)
        adv0[i] = timer_advances(i);
    struct timer_stats st0, st1;
    timer_get_stats(&st0);

    int tid = thread_create_prio("cs-sleeper", cs_sleeper, NULL, PRIO_HIGH);
    if (tid <= 0)
        panic("cpu0-stall: cannot create sleeper (%d)", tid);
    thread_sleep(5);                         /* 讓它在別顆 CPU 上跑起來、進入忙等 */

    /* 讓第 0 顆 CPU 關中斷空轉 300 毫秒。沒有「把執行緒固定在某顆 CPU」的機制,以前靠反覆睡 1 毫秒
       碰運氣被排到第 0 顆,24 次有 3 次一直排不上而逾時。現在:不在第 0 顆就以跨 CPU 呼叫讓第 0 顆
       在 IPI 處理中(本來就關中斷)執行空轉;在第 0 顆就直接關中斷空轉。兩條路都確定是第 0 顆卡住。
       跨 CPU 呼叫的發起者等待時也關著中斷,所以那段期間停住的是第 0 顆加發起者 — 需要至少 3 顆 CPU */
    struct cs_stall_ctx ctx;
    int attempts = 0;
    do {
        if (++attempts > 50)
            panic("cpu0-stall: the sleeper kept running on cpu 0");
        for (int w = 0; cs_sleeper_cpu < 1; w++) {   /* 等睡眠者確實在第 0 顆以外的 CPU 上忙等 */
            if (w > 5000)
                panic("cpu0-stall: the sleeper never settled off cpu 0");
            thread_sleep(1);
        }
        for (unsigned i = 0; i < sizeof(ctx); i++)
            ((char *)&ctx)[i] = 0;
        if (sched_cpu_id() == 0) {
            unsigned long fl = arch_irq_save();
            cs_stall_fn(&ctx);
            arch_irq_restore(fl);
        } else {
            smp_call_mask(1u << 0, cs_stall_fn, &ctx);
        }
    } while (ctx.aborted);
    if (!ctx.ran_on_cpu0)
        panic("cpu0-stall: the stall did not run on cpu 0");
    unsigned long hb0 = ctx.hb0, hb1 = ctx.hb1;
    uint64_t tk0 = ctx.tk0, tk1 = ctx.tk1, c1 = ctx.c1;
    st1 = ctx.st1;
    cs_phase = 2;
    thread_join(tid);

    /* 只評估「整段都落在卡住期間」的睡眠:最後一次跨過結束點,不算 */
    unsigned wakes = 0, strict_over = 0;
    uint64_t worst = 0;
    for (unsigned i = 0; i < cs_n; i++) {
        if (cs_t0[i] + cs_d[i] > c1)
            continue;
        wakes++;
        if (cs_d[i] > worst)
            worst = cs_d[i];
        if (cs_d[i] * 1000 > cs_hz * CS_STRICT_MS)
            strict_over++;
    }

    uint32_t others = 0;
    for (int i = 1; i < MAX_CPUS; i++)
        others += timer_advances(i) - adv0[i];
    uint32_t adv_cpu0 = timer_advances(0) - adv0[0];
    struct timer_stats dst;
    for (int b = 0; b < TIMER_BURST_BUCKETS; b++)
        dst.burst[b] = st1.burst[b] - st0.burst[b];
    dst.max_burst = 0;
    dst.resyncs = st1.resyncs - st0.resyncs;
    uint32_t host_stalls = timer_bursts_at_least(&dst, 10);
    kprintf("cpu0-stall: cpu0 irqs off %u ms: ticks +%llu, cpu0 heartbeat +%lu, advances cpu0 %u others %u; "
            "sleeper woke %u times, worst %llu counts (clock %llu Hz), %u over %u ms, host stalls >=10 ticks %u\n",
            CS_STALL_MS, (unsigned long long)(tk1 - tk0), hb1 - hb0, adv_cpu0, others, wakes,
            (unsigned long long)worst, (unsigned long long)cs_hz, strict_over, CS_STRICT_MS, host_stalls);

    if (hb1 != hb0)
        panic("cpu0-stall: control failed, cpu0 handled %lu timer interrupts with irqs off", hb1 - hb0);
    if ((tk1 - tk0) * 1000 < (uint64_t)timer_hz() * (CS_STALL_MS - CS_HARD_MS / 2))
        panic("cpu0-stall: system time advanced only %llu ticks while cpu0 was stalled %u ms",
              (unsigned long long)(tk1 - tk0), CS_STALL_MS);
    if (others == 0)
        panic("cpu0-stall: no other cpu advanced system time");
    if (cs_on_cpu0)
        panic("cpu0-stall: sleeper ran on cpu0 while its irqs were off (%u)", cs_on_cpu0);
    if (wakes < CS_STALL_MS / CS_HARD_MS)  /* 修正前:睡眠全部跨過結束點,這裡是 0 */
        panic("cpu0-stall: sleeper woke only %u times during the stall", wakes);
    if (worst * 1000 > cs_hz * CS_HARD_MS)
        panic("cpu0-stall: a %u ms sleep took %llu counts (> %u ms, clock %llu Hz)", CS_SLEEP_MS,
              (unsigned long long)worst, CS_HARD_MS, (unsigned long long)cs_hz);
    /* 設計上限只在「這段期間模擬器沒有停頓」時可驗證:停頓本身就是所有 CPU 都沒有在跑 */
    if (strict_over && host_stalls == 0)
        panic("cpu0-stall: %u sleeps exceeded the %u ms bound without any host stall",
              strict_over, CS_STRICT_MS);
}

/*
 * ---- 跨 CPU 計數器同步驗證(I-337 / Codex 209、210)----
 * 1. 開機驗證與「至少 1 秒後」的再驗證都必須合格,印出每顆 CPU 的最大偏移(千分之一 tick)。
 * 2. 負對照(證明拒絕路徑真的會被走到,不是永遠回答合格):
 *    a. 讓一顆 CPU 回報的計數超前 4 倍容許值 → 必須以 OFFSET 拒絕,而且指名那一顆
 *    b. 讓它讀完後拖延半個 tick 才回應 → 括號全部太寬 → 必須以 WIDE 拒絕
 *    c. 清除注入 → 必須合格
 *    三次都是乾跑(clksync_run_dry):不改變系統時間的模式,做完共用路徑必須仍然啟用。
 */
void clksync_test(void)
{
    int n = smp_started();
    if (n < 2) {
        KASSERT(clksync_state() == CLKSYNC_SINGLE);
        kprintf("clksync-test: 1 cpu, skipped\n");
        return;
    }
    if (timer_clock_hz() == 0 || timer_tick_counts() == 0) {
        /* 沒有共用計數器的機器必須已經降級,而且共用路徑沒有啟用 */
        KASSERT(clksync_state() == CLKSYNC_DEGRADED && !timer_shared_enabled());
        kprintf("clksync-test: no shared counter, degraded as required\n");
        return;
    }
    clksync_recheck();                       /* 開機後不到 1 秒時會睡到滿 1 秒 */
    if (clksync_state() != CLKSYNC_OK) {
        const struct clksync_result *bad = clksync_last(1) ? clksync_last(1) : clksync_last(0);
        panic("clksync-test: shared counter rejected: %s (cpu %d)",
              bad ? clksync_reason_name(bad->reason) : "never checked", bad ? bad->bad_cpu : -1);
    }
    const struct clksync_result *r0 = clksync_last(0), *r1 = clksync_last(1);
    KASSERT(r0 && r1 && r0->ok && r1->ok && timer_shared_enabled());
    for (int c = 0; c < n; c++) {
        if (c == r0->ref)
            continue;
        uint64_t a = r0->cpu[c].max_abs;
        uint64_t b = (c == r1->ref) ? 0 : r1->cpu[c].max_abs;   /* 再驗證可能在別顆 CPU 上做 */
        kprintf("clksync-test: cpu %d max offset %u/1000 tick at boot, %u/1000 tick after recheck "
                "(limit 250/1000)\n", c, clksync_milli_ticks(a, r0->iv), clksync_milli_ticks(b, r1->iv));
    }

    static struct clksync_result t;          /* 不放在 stack:結構約 8 × 64 位元組 */
    uint64_t tol = timer_tick_counts() >> 2;
    /* 關中斷固定在同一顆 CPU 上,選一顆「不是自己」的 CPU 當注入對象(自己是參考,不會被取樣) */
    unsigned long fl = arch_irq_save();
    int victim = this_cpu()->id == 0 ? 1 : 0;
    clksync_test_inject(victim, tol * 4, 0);
    int ok_off = clksync_run_dry(&t);
    int r_off = t.reason, c_off = t.bad_cpu;
    clksync_test_inject(victim, 0, tol * 2);
    int ok_wide = clksync_run_dry(&t);
    int r_wide = t.reason, c_wide = t.bad_cpu;
    clksync_test_inject(-1, 0, 0);
    int ok_clean = clksync_run_dry(&t);
    arch_irq_restore(fl);

    kprintf("clksync-test: negative control on cpu %d: offset +%llu counts -> %s, "
            "reply delay %llu counts -> %s, clean -> %s\n", victim,
            (unsigned long long)(tol * 4), ok_off ? "ACCEPTED" : clksync_reason_name(r_off),
            (unsigned long long)(tol * 2), ok_wide ? "ACCEPTED" : clksync_reason_name(r_wide),
            ok_clean ? "accepted" : clksync_reason_name(t.reason));
    if (ok_off || r_off != CLKSYNC_R_OFFSET || c_off != victim)
        panic("clksync-test: injected offset not rejected as OFFSET on cpu %d (ok %d reason %d cpu %d)",
              victim, ok_off, r_off, c_off);
    if (ok_wide || r_wide != CLKSYNC_R_WIDE || c_wide != victim)
        panic("clksync-test: injected delay not rejected as WIDE on cpu %d (ok %d reason %d cpu %d)",
              victim, ok_wide, r_wide, c_wide);
    if (!ok_clean)
        panic("clksync-test: clean dry run rejected: %s (cpu %d)", clksync_reason_name(t.reason), t.bad_cpu);
    if (clksync_state() != CLKSYNC_OK || !timer_shared_enabled())
        panic("clksync-test: a dry run changed the time mode");
}
