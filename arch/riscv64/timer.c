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
#include "irq_late.h"
#include "lockhold.h"
#include "cpu.h"
#include "timer.h"
#include "sched.h"
#include "trap.h"
#include "lockhold_model.h"

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

uint64_t arch_clock_count(void)
{
    return read_time();
}

/* RISC-V 的 CSR 存取預設與記憶體存取是弱排序(Zicsr)。以 fence iorw,iorw 夾住 time 的讀取:
   之前的記憶體讀取(例如看到 ack)完成後才讀,讀完之後才做之後的存取。這是盡力而為的做法 —
   規格沒有明說 fence 對 time CSR 的效果,實測以計數器同步檢查的結果佐證(I-342) */
uint64_t arch_clock_count_ordered(void)
{
    __asm__ volatile("fence iorw, iorw" ::: "memory");
    uint64_t t = read_time();
    __asm__ volatile("fence iorw, iorw" ::: "memory");
    return t;
}

uint64_t arch_clock_hz(void)
{
    return TIMEBASE_HZ;
}

uint64_t arch_tick_interval(void)
{
    return interval;
}

/* I-341:time CSR 是架構上全系統共用的單一時基,沒有「另一個」共用計數器可以退回(見 arch.h) */
int arch_clock_fallback(void)
{
    return 0;
}

/* I-349 鎖持有量測的戳記:time CSR 一開機就可讀(S 模式由韌體開放),來源固定、不會切換。
   source 1 = time CSR(本架構唯一的來源),generation 固定 0 */
void arch_lh_stamp(struct lh_stamp *out)
{
    /* 編譯器記憶體屏障(Codex 092):讀數不被編譯器搬到 hook 前後的記憶體存取之外;硬體層面仍未序列化(STAMP_UNSERIALIZED) */
    __asm__ volatile("" ::: "memory");
    out->count = read_time();
    __asm__ volatile("" ::: "memory");
    out->source = 1;
    out->generation = 0;
    out->valid = 1;
}

const char *arch_clock_name(void)
{
    return "time";
}

void arch_clock_test_mode(int mode)
{
    (void)mode;
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
    interval = (TIMEBASE_HZ + hz - 1) / hz;  /* 無條件進位:tick 只可以比真實時間慢,不可以快(見 timer.h) */
    arch_timer_init_cpu();
}

/* 啟動「目前這顆 hart」的計時器。sbi_set_timer 與 sie 都是每顆 hart 各自的 */
void arch_timer_init_cpu(void)
{
    deadline = read_time() + interval;
    sbi_set_timer(deadline);
    arch_irq_late_config_cpu(this_cpu()->id, interval);   /* I-351:deadline 與 time CSR 同單位 */
    __asm__ volatile("csrs sie, %0" : : "r"(SIE_STIE));
}

/* I-351:time CSR 是唯一的共用計數器(src_id 0),每顆 hart 都有預定 deadline */
void arch_irq_late_config_cpu(int cpu, uint64_t iv)
{
    irq_late_config(cpu, 0, IRQL_KIND_DEADLINE, iv);
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
    lockhold_safepoint();                    /* 鎖持有量測的安全點:尚未取 time_lk / sched_lk */
    sched_irq_enter();                       /* 中斷中喚醒別人時,切換要留到返回前 */

    uint64_t now = read_time();          /* = arch_clock_count() = timer_clock_count() 的讀值 */
    uint64_t ref = deadline;             /* I-351:補記之前的預定 deadline */
    int n = 0;

    while (deadline <= now && n < MAX_CATCHUP) {
        if (n == 0)
            timer_tick();
        else
            timer_tick_stolen();         /* 補記的:不算成誰的 CPU 時間 */
        deadline += interval;
        n++;
    }
    int resynced = deadline <= now;
    if (resynced)                     /* 落後太多(例如被除錯器暫停):放棄補記,重新對時 */
        deadline = now + interval;
    timer_note_catchup(n, resynced);   /* 晚到的紀錄(見 timer.h) */
    struct irq_late_in li = { IRQL_KIND_DEADLINE, resynced ? IRQL_F_RESYNC : 0, (uint16_t)n, ref, now, 0 };   /* src_id 0:唯一的共用計數器 */
    irq_late_note(&li);                /* I-351:獨立的晚到觀測,不影響上面的補記 */

    sbi_set_timer(deadline);                 /* 設定下一次,並清除本次的待處理狀態 */
    sched_irq_exit();                        /* 時間片用完就在這裡換人(下一次鬧鐘已設好) */
}
