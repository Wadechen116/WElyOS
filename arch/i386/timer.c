/*
 * timer.c (i386) - 8259 PIC 中斷控制器與 8253/8254 PIT 計時器
 *
 * PC 的硬體中斷(IRQ)經由兩顆串接的 8259 PIC 送到 CPU:
 *   主 PIC(port 0x20/0x21):IRQ 0~7      從 PIC(port 0xA0/0xA1):IRQ 8~15
 *   從 PIC 接在主 PIC 的 IRQ 2 上。
 *
 * 為什麼要「重新對映」:開機時 BIOS 把 IRQ 0~7 對應到向量 8~15,
 * 但保護模式下向量 0~31 保留給 CPU 例外(例如 8 是 double fault)。
 * 不改的話,計時器中斷(IRQ 0)會被當成 double fault。
 * 所以把 IRQ 0~15 改對應到向量 32~47。
 *
 * PIT 是可程式化計時器,輸入時脈固定 1193182 Hz。channel 0 接在 IRQ 0,
 * 設定除數 N 後以 1193182 / N 的頻率產生中斷(週期性,不用每次重設)。
 *
 * 計時的準確性:PIT 只會「發出中斷」,不會告訴我們現在是第幾次。如果中斷
 * 被延遲到下一次也到了(中斷關閉太久,或模擬器的主機排程延遲),兩次會合併
 * 成一次,kernel 少算一個 tick,系統時間就變慢,而且無從得知。
 * 所以另外用 TSC(time stamp counter,CPU 內建、隨時間遞增的 64 位元計數器)
 * 當作獨立的時間來源:PIT 中斷只負責「叫醒」,實際該記幾個 tick 由 TSC
 * 經過了多少時間來決定。CPU 不支援 TSC 時退回每次中斷記一個 tick。
 */
#include <stdint.h>
#include "arch.h"
#include "io.h"
#include "kprintf.h"
#include "timer.h"
#include "sched.h"
#include "trap.h"

#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1
#define PIC_EOI    0x20        /* End Of Interrupt 命令 */

#define PIT_CH0    0x40
#define PIT_CMD    0x43
#define PIT_HZ     1193182u

#define IRQ_BASE   32          /* IRQ 0 對應的向量編號 */

#define PIT_CH2    0x42        /* channel 2:原本接喇叭,輸出狀態可由 port 0x61 讀回 */
#define PORT_SPKR  0x61        /* bit 0 = channel 2 的閘門,bit 1 = 喇叭開關,bit 5 = channel 2 輸出 */
#define MAX_CATCHUP 100        /* 一次中斷最多補記幾個 tick */

static uint64_t tsc_interval;  /* 一個 tick 相當於多少 TSC 計數;0 = 不使用 TSC */
static uint64_t tsc_deadline;  /* 下一個 tick 應該發生的 TSC 值 */

static uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* CPUID 功能位元:EDX bit 4 = 支援 TSC */
static int cpu_has_tsc(void)
{
    uint32_t eax = 1, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    (void)ebx; (void)ecx;
    return (edx >> 4) & 1;
}

/*
 * 量測 TSC 的頻率:TSC 每秒跳幾次因 CPU 而異,必須用一個已知頻率的時鐘校正。
 * 讓 PIT 的 channel 2 倒數 10ms(單次模式),輪詢它的輸出直到變成 1,
 * 這段期間 TSC 增加的量就是「10ms 的 TSC 計數」。
 * 不用 channel 0 是因為它接著 IRQ 0,而此時中斷還沒開。
 */
static uint32_t tsc_per_10ms(void)
{
    uint16_t count = PIT_HZ / 100;                       /* 10ms */

    outb(PORT_SPKR, (uint8_t)((inb(PORT_SPKR) & ~0x02) | 0x01));   /* 開閘門、關喇叭 */
    outb(PIT_CMD, 0xB0);                 /* channel 2、先低後高、模式 0(倒數到 0 後輸出變 1) */
    outb(PIT_CH2, (uint8_t)(count & 0xFF));
    outb(PIT_CH2, (uint8_t)(count >> 8));

    uint64_t t0 = rdtsc();
    while (!(inb(PORT_SPKR) & 0x20))
        ;
    return (uint32_t)(rdtsc() - t0);     /* 10ms 內的計數,32 位元足夠 */
}

uint64_t arch_tsc_per_tick(void)
{
    return tsc_interval;
}

static void pic_remap(void)
{
    /* ICW1:開始初始化序列(之後要連續寫入 ICW2~ICW4) */
    outb(PIC1_CMD, 0x11);
    outb(PIC2_CMD, 0x11);
    /* ICW2:向量基底 */
    outb(PIC1_DATA, IRQ_BASE);        /* IRQ 0~7  -> 向量 32~39 */
    outb(PIC2_DATA, IRQ_BASE + 8);    /* IRQ 8~15 -> 向量 40~47 */
    /* ICW3:串接方式 */
    outb(PIC1_DATA, 0x04);            /* 主 PIC:IRQ 2 接著從 PIC(位元遮罩) */
    outb(PIC2_DATA, 0x02);            /* 從 PIC:自己接在主 PIC 的 IRQ 2(編號) */
    /* ICW4:8086 模式 */
    outb(PIC1_DATA, 0x01);
    outb(PIC2_DATA, 0x01);
    /* 中斷遮罩(1 = 關閉):只開 IRQ 0(計時器),其餘全部關閉 */
    outb(PIC1_DATA, 0xFE);
    outb(PIC2_DATA, 0xFF);
}

void arch_timer_init(unsigned hz)
{
    uint32_t divisor = PIT_HZ / hz;   /* 16 位元除數,hz 至少要 19 */

    pic_remap();
    /* 0x36:channel 0、先低後高位元組、模式 3(方波,週期性)、二進位計數 */
    outb(PIT_CMD, 0x36);
    outb(PIT_CH0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH0, (uint8_t)(divisor >> 8));

    if (cpu_has_tsc()) {
        uint32_t d = tsc_per_10ms();
        /* 換算成一個 tick 的計數:d * 100 / hz。拆成兩步避免 32 位元溢位,
           也避開 64 位元除法(i386 上需要 libgcc 的 __udivdi3) */
        tsc_interval = (uint64_t)(d / hz) * 100 + (d % hz) * 100 / hz;
        tsc_deadline = rdtsc() + tsc_interval;
    }
}

/* 由 trap_handler 在向量 32~47 時呼叫 */
void irq_handler(uint32_t irq)
{
    sched_irq_enter();                       /* 中斷中喚醒別人時,切換要留到返回前 */

    if (irq == 0) {
        if (tsc_interval) {
            /* 依 TSC 實際經過的時間決定要記幾個 tick(通常是 1 個;
               中斷被合併時會是多個,把漏掉的補回來) */
            uint64_t now = rdtsc();
            int n = 0;
            while (tsc_deadline <= now && n < MAX_CATCHUP) {
                timer_tick();
                tsc_deadline += tsc_interval;
                n++;
            }
            if (tsc_deadline <= now)     /* 落後太多:放棄補記,重新對時 */
                tsc_deadline = now + tsc_interval;
        } else {
            timer_tick();
        }
    } else
        kprintf("[irq] unexpected IRQ %u\n", irq);

    /* 通知 PIC 處理完畢,它才會再送下一個中斷;來自從 PIC 的要兩邊都通知 */
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);

    /* 必須在 EOI 之後:這裡可能切換到別的執行緒,很久才回來。
       若還沒 EOI 就切走,PIC 會一直不送下一個計時器中斷 */
    sched_irq_exit();
}
