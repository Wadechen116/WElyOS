/*
 * timer.c - 系統時鐘(與架構無關)
 *
 * 架構層讓硬體計時器每秒產生 hz 次中斷,每次中斷呼叫 timer_tick()。
 * tick 計數是 kernel 的時間基準,之後的排程器也靠它切換工作。
 */
#include "arch.h"
#include "cpu.h"
#include "sched.h"
#include "timer.h"

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
    sched_tick();              /* 喚醒休眠者、扣時間片(排程器未啟動時無作用) */
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
