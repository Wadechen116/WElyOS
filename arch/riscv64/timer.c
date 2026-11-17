/*
 * timer.c (RISC-V 64) - 透過 SBI 使用計時器
 *
 * RISC-V 的計時器(mtime / mtimecmp)屬於 M-mode,S-mode 的 kernel 不能直接設定,
 * 要用 ecall 請 M-mode 韌體(OpenSBI)代勞,這套介面稱為 SBI。
 *
 * 運作方式是「單次鬧鐘」而非週期性:
 *   1. 讀取目前時間(time CSR,單位為 timebase 的 tick)
 *   2. sbi_set_timer(現在 + 間隔):到時會產生 S-mode 計時器中斷(scause 中斷碼 5)
 *   3. 中斷處理程式中再設定下一次,如此循環
 * 呼叫 sbi_set_timer 同時會清除目前待處理的計時器中斷。
 */
#include <stdint.h>
#include "arch.h"
#include "cpu.h"
#include "timer.h"
#include "sched.h"
#include "trap.h"

/*
 * time CSR 每秒增加的次數。正確做法是從裝置樹 /cpus 的 timebase-frequency 讀取;
 * QEMU virt 固定為 10MHz,先寫死(實機需改為讀取裝置樹)。
 */
#define TIMEBASE_HZ 10000000UL

#define SBI_EXT_TIME      0x54494D45   /* "TIME" 的 ASCII:SBI 計時器擴充 */
#define SBI_FN_SET_TIMER  0

#define SIE_STIE (1UL << 5)            /* sie 的 bit 5:允許 S-mode 計時器中斷 */

static uint64_t interval;              /* 每個 tick 相當於多少 timebase 單位 */
/* 下一個 tick 應該發生的絕對時間:每顆 hart 有自己的計時器,
   所以記在各自的 struct cpu(timer_deadline)裡 */
#define deadline (this_cpu()->timer_deadline)

#define MAX_CATCHUP 100                /* 一次中斷最多補記幾個 tick */

static uint64_t read_time(void)
{
    uint64_t t;
    __asm__ volatile("csrr %0, time" : "=r"(t));
    return t;
}

/* SBI 呼叫慣例:a7 = 擴充 ID,a6 = 功能 ID,a0.. = 參數;ecall 後 a0 = 錯誤碼 */
static void sbi_set_timer(uint64_t when)
{
    register uint64_t a0 __asm__("a0") = when;
    register uint64_t a6 __asm__("a6") = SBI_FN_SET_TIMER;
    register uint64_t a7 __asm__("a7") = SBI_EXT_TIME;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a6), "r"(a7) : "a1", "memory");
}

void arch_timer_init(unsigned hz)
{
    interval = TIMEBASE_HZ / hz;
    arch_timer_init_cpu();
}

/* 啟動「目前這顆 hart」的計時器。sbi_set_timer 與 sie 都是每顆 hart 各自的 */
void arch_timer_init_cpu(void)
{
    deadline = read_time() + interval;
    sbi_set_timer(deadline);
    __asm__ volatile("csrs sie, %0" : : "r"(SIE_STIE));
}

/*
 * 由 trap_handler 在收到 S-mode 計時器中斷時呼叫。
 *
 * 下一次的時間用「上一次的預定時間 + 間隔」計算,而不是「現在 + 間隔」。
 * 中斷實際送達的時間總會比預定晚一點;若每次都以「現在」為基準,
 * 這些延遲會一直累加,時鐘越走越慢。以預定時間為基準就不會累積誤差。
 *
 * 如果晚了不只一個間隔(中斷被關閉太久,或模擬器的主機排程延遲),
 * 就把錯過的 tick 一次補記回來,系統時間才不會落後。
 */
void timer_interrupt(void)
{
    sched_irq_enter();                       /* 中斷中喚醒別人時,切換要留到返回前 */

    uint64_t now = read_time();
    int n = 0;

    while (deadline <= now && n < MAX_CATCHUP) {
        timer_tick();
        deadline += interval;
        n++;
    }
    if (deadline <= now)                     /* 落後太多(例如被除錯器暫停):放棄補記,重新對時 */
        deadline = now + interval;

    sbi_set_timer(deadline);                 /* 設定下一次,並清除本次的待處理狀態 */
    sched_irq_exit();                        /* 時間片用完就在這裡換人(下一次鬧鐘已設好) */
}
