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
