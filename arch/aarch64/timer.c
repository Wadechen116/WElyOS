/*
 * timer.c (AArch64) - GICv2 中斷控制器與 ARM generic timer
 *
 * 與 RISC-V 不同,ARM 的週邊中斷都要經過中斷控制器 GIC 才會送到 CPU:
 *
 *   計時器 --(中斷號 30)--> GIC distributor --> GIC CPU interface --> CPU 的 IRQ
 *
 * GIC 分兩部分(QEMU virt、GICv2 的位址):
 *   Distributor   0x08000000:管理每個中斷號的開關、優先權
 *   CPU interface 0x08010000:每個 CPU 一份;回報是哪個中斷(IAR)、通知處理完畢(EOIR)
 *
 * ARM generic timer 是 CPU 內建的計時器,以系統暫存器操作:
 *   CNTFRQ_EL0    計數頻率(Hz)
 *   CNTP_TVAL_EL0 倒數值:寫入 N 表示 N 個計數後觸發
 *   CNTP_CTL_EL0  控制:bit 0 啟用,bit 1 遮蔽輸出
 * EL1 實體計時器的中斷號是 30(屬於每個 CPU 私有的 PPI)。
 * 同樣是「單次鬧鐘」,中斷處理程式中要重新寫入倒數值。
 */
#include <stdint.h>
#include "arch.h"
#include "kprintf.h"
#include "panic.h"
#include "sched.h"
#include "timer.h"
#include "trap.h"

#define GICD_BASE 0x08000000UL
#define GICC_BASE 0x08010000UL

#define GICD_CTLR        0x000     /* distributor 總開關 */
#define GICD_ISENABLER0  0x100     /* 中斷 0~31 的啟用位元(寫 1 啟用) */
#define GICD_IPRIORITYR  0x400     /* 每個中斷 1 位元組的優先權(數值越小越優先) */

#define GICC_CTLR        0x000     /* CPU interface 開關 */
#define GICC_PMR         0x004     /* 優先權門檻:優先權數值低於它的中斷才會送達 */
#define GICC_IAR         0x00C     /* 讀取:取得待處理的中斷號(並標記為處理中) */
#define GICC_EOIR        0x010     /* 寫入:通知該中斷處理完畢 */

#define IRQ_TIMER        30        /* EL1 實體計時器 */
#define IRQ_SPURIOUS     1023      /* 沒有待處理中斷時 IAR 的回傳值 */

static inline void mmio_w32(uintptr_t addr, uint32_t v)
{
    *(volatile uint32_t *)addr = v;
}

static inline uint32_t mmio_r32(uintptr_t addr)
{
    return *(volatile uint32_t *)addr;
}

static uint64_t interval;          /* 每個 tick 的計數值 */

void arch_timer_init(unsigned hz)
{
    uint64_t freq;

    /* GIC:啟用 distributor 與 CPU interface,打開計時器的中斷號 */
    mmio_w32(GICD_BASE + GICD_CTLR, 1);
    *(volatile uint8_t *)(GICD_BASE + GICD_IPRIORITYR + IRQ_TIMER) = 0xA0;
    mmio_w32(GICD_BASE + GICD_ISENABLER0, 1u << IRQ_TIMER);
    mmio_w32(GICC_BASE + GICC_PMR, 0xFF);        /* 門檻最寬:接受所有優先權 */
    mmio_w32(GICC_BASE + GICC_CTLR, 1);

    /* 計時器:依頻率算出每個 tick 的計數,寫入倒數值並啟用 */
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    interval = freq / hz;
    __asm__ volatile("msr cntp_tval_el0, %0" : : "r"(interval));
    __asm__ volatile("msr cntp_ctl_el0, %0" : : "r"(1UL));
}

/* 由 trap_handler 在收到 IRQ 時呼叫 */
void irq_handler(void)
{
    uint32_t iar = mmio_r32(GICC_BASE + GICC_IAR);   /* 認領中斷 */
    uint32_t id = iar & 0x3FF;

    if (id == IRQ_SPURIOUS)
        return;

    if (id == IRQ_TIMER) {
        /* 重新寫入倒數值:同時讓計時器的中斷訊號消失 */
        __asm__ volatile("msr cntp_tval_el0, %0" : : "r"(interval));
        timer_tick();
    } else {
        kprintf("[irq] unexpected interrupt %u\n", id);
    }

    mmio_w32(GICC_BASE + GICC_EOIR, iar);            /* 處理完畢,GIC 才會再送下一個 */

    /* 必須在 EOI 之後:這裡可能切換到別的執行緒,很久才回來 */
    sched_irq_exit();
}
