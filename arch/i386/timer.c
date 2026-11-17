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
}

/* 由 trap_handler 在向量 32~47 時呼叫 */
void irq_handler(uint32_t irq)
{
    if (irq == 0)
        timer_tick();
    else
        kprintf("[irq] unexpected IRQ %u\n", irq);

    /* 通知 PIC 處理完畢,它才會再送下一個中斷;來自從 PIC 的要兩邊都通知 */
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);

    /* 必須在 EOI 之後:這裡可能切換到別的執行緒,很久才回來。
       若還沒 EOI 就切走,PIC 會一直不送下一個計時器中斷 */
    sched_irq_exit();
}
