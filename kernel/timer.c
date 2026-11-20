/*
 * timer.c - 系統時鐘(與架構無關)
 *
 * 架構層讓硬體計時器每秒產生 hz 次中斷,每次中斷呼叫 timer_tick()。
 * tick 計數是 kernel 的時間基準,之後的排程器也靠它切換工作。
 */
#include "arch.h"
#include "cpu.h"
#include "sched.h"
#include "spinlock.h"
#include "timer.h"
#include "watchdog.h"

/* volatile:在中斷處理程式中被修改,主程式每次都必須重新讀取 */
static volatile uint64_t ticks;
static unsigned tick_hz;

void timer_init(unsigned hz)
{
    tick_hz = hz;
    arch_timer_init(hz);
}

void timer_tick(void)
{
    /* 每顆 CPU 都有自己的計時器中斷,但系統時間只由第 0 顆 CPU 推進,
       否則 N 顆 CPU 時間就會走快 N 倍 */
    if (this_cpu()->id == 0)
        ticks++;
    watchdog_tick();           /* 心跳,並看看隔壁的 CPU 是不是還活著 */
    sched_tick(1);             /* 喚醒休眠者、扣時間片(排程器未啟動時無作用) */
}

void timer_tick_stolen(void)
{
    if (this_cpu()->id == 0)
        ticks++;
    watchdog_tick();
    sched_tick(0);             /* 只推進時間:不記 CPU 時間、不扣時間片與即時工作的預算 */
}

/*
 * 32 位元架構(i386)讀取 64 位元變數需要兩條指令,中間若剛好發生
 * 計時器中斷,會讀到高低半部不一致的值。連讀兩次直到相同即可避免。
 */
uint64_t timer_ticks(void)
{
    uint64_t a, b;
    do {
        a = ticks;
        b = ticks;
    } while (a != b);
    return a;
}

/* 晚到的紀錄(見 timer.h)。中斷處理程式(各 CPU 都可能)寫入,以一把 spinlock 保護;只有幾條指令 */
static struct timer_stats tstats;
static struct spinlock tstat_lk = SPINLOCK_INIT("timer-stats", LK_TSTAT);
static const unsigned bucket_lo[TIMER_BURST_BUCKETS] = { 2, 3, 4, 6, 7, 10 };

void timer_note_catchup(int n, int resynced)
{
    if (n < 2 && !resynced)
        return;
    unsigned b = TIMER_BURST_BUCKETS - 1;
    if (!resynced)
        while (b > 0 && (unsigned)n < bucket_lo[b])
            b--;
    unsigned long fl = spin_lock_irqsave(&tstat_lk);
    tstats.burst[b]++;
    if (resynced)
        tstats.resyncs++;
    else if ((uint32_t)n > tstats.max_burst)
        tstats.max_burst = (uint32_t)n;
    spin_unlock_irqrestore(&tstat_lk, fl);
}

void timer_get_stats(struct timer_stats *st)
{
    unsigned long fl = spin_lock_irqsave(&tstat_lk);
    *st = tstats;
    spin_unlock_irqrestore(&tstat_lk, fl);
}

uint32_t timer_bursts_at_least(const struct timer_stats *st, unsigned k)
{
    uint32_t sum = 0;

    for (unsigned b = 0; b < TIMER_BURST_BUCKETS; b++)
        if (bucket_lo[b] >= k)
            sum += st->burst[b];
    return sum;
}

unsigned timer_hz(void)
{
    return tick_hz;
}

/*
 * ---- 實際的日期時間 ----
 *
 * tick 只知道「開機之後過了多久」。要知道「現在是幾月幾號幾點」,得問硬體的即時時鐘
 * (RTC):主機板上一個有電池的小時鐘,關機時也在走。讀它比較慢(i386 要好幾次 I/O),
 * 所以只在開機時問一次,記下「開機那一刻是第幾秒」,之後的時間 = 那一刻 + 開機後經過的時間。
 */
static uint64_t boot_epoch;                  /* 開機那一刻的 Unix 時間(秒) */

/* 64 位元除以 32 位元。不直接寫 n / d:i386 上那需要編譯器的輔助函式庫(見 sched.c 的說明)。
   每次處理 16 個位元,被除數因此放得進 64 位元的一半;d 不可超過 65535 */
static uint64_t div_u64(uint64_t n, uint32_t d, uint32_t *rem)
{
    uint64_t q = 0;
    uint32_t r = 0;

    for (int shift = 48; shift >= 0; shift -= 16) {
        uint32_t part = (r << 16) | (uint32_t)((n >> shift) & 0xFFFF);
        q |= (uint64_t)(part / d) << shift;
        r = part % d;
    }
    if (rem)
        *rem = r;
    return q;
}

void timer_clock_init(void)
{
    uint64_t now = arch_rtc_read();
    uint64_t up = div_u64(timer_ticks(), tick_hz ? tick_hz : 1, NULL);

    boot_epoch = now > up ? now - up : 0;
}

uint64_t timer_epoch(uint32_t *usec)
{
    uint32_t rem = 0;
    uint64_t sec = div_u64(timer_ticks(), tick_hz ? tick_hz : 1, &rem);

    if (usec)
        *usec = tick_hz ? rem * (1000000u / tick_hz) : 0;
    return boot_epoch + sec;
}

/* ---- 開機時間的記號 ---- */
static uint64_t mark_kmain, mark_ready;

void timer_clock_mark(int which)
{
    uint64_t *m = which == CLOCK_MARK_KMAIN ? &mark_kmain : &mark_ready;
    if (*m == 0)
        *m = arch_clock_count();
}

void timer_clock_marks(uint64_t *kmain, uint64_t *ready)
{
    *kmain = mark_kmain;
    *ready = mark_ready;
}

uint64_t timer_clock_count(void)
{
    return arch_clock_count();
}

uint64_t timer_clock_hz(void)
{
    return arch_clock_hz();
}

/* 一次最多補算幾個 tick:和架構層中斷裡的補記上限相同(超過時架構層放棄補記、重新對時,
   那些 tick 永遠不會被記上;這裡也不算它們,兩邊才一致) */
#define REAL_CATCHUP_MAX 100

uint64_t timer_ticks_real(void)
{
    /* 先讀預定時間、再讀 tick 計數(寫入端的順序相反,見 arch.h):
       讀到新的預定時間就一定讀到新的計數;交錯時頂多多算 1 個 tick — 只會讓等待變長,不會變短 */
    uint64_t d = arch_tick_deadline();
    uint64_t t = timer_ticks();
    uint64_t iv = arch_tick_interval();
    if (d == 0 || iv == 0)
        return t;
    uint64_t now = arch_clock_count();
    unsigned k = 0;
    while (now >= d && k < REAL_CATCHUP_MAX) {   /* 每一個已經過去的預定時間 = 一個該記而還沒記的 tick */
        d += iv;
        k++;
    }
    return t + k;
}

uint64_t timer_deadline_ms(unsigned ms)
{
    if (ms == 0)
        return 0;
    /* 現在位在第 r 個 tick 之中的某處;到第 r + n 個 tick 時,真實經過的時間至少 n - 1 個 tick,
       所以 n = 無條件進位的 tick 數 + 1(與 thread_sleep 相同;32 位元運算,避開 i386 的 __udivdi3) */
    unsigned n = (ms * timer_hz() + 999) / 1000 + 1;
    return timer_ticks_real() + n;
}

int timer_expired(uint64_t deadline)
{
    return timer_ticks() >= deadline;
}
