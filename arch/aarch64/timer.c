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
 *   CNTPCT_EL0    目前的計數值(只會遞增)
 *   CNTP_CVAL_EL0 比較值:計數值到達這個絕對時間就觸發
 *   CNTP_CTL_EL0  控制:bit 0 啟用,bit 1 遮蔽輸出
 * EL1 實體計時器的中斷號是 30(屬於每個 CPU 私有的 PPI)。
 * 同樣是「單次鬧鐘」,中斷處理程式中要寫入下一次的比較值。
 */
#include <stdint.h>
#include "cpu.h"
#include "smp.h"
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

#define GICD_SGIR        0xF00     /* 寫入:產生軟體中斷(SGI),也就是處理器間中斷 */

#define IRQ_IPI          1         /* 用第 1 號 SGI 當作處理器間中斷(SGI 為 0~15) */
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
/* 下一個 tick 應該發生的絕對時間:每顆 CPU 有自己的計時器,記在各自的 struct cpu 裡 */
#define deadline (this_cpu()->timer_deadline)

#define MAX_CATCHUP 100            /* 一次中斷最多補記幾個 tick */

static uint64_t read_counter(void)
{
    uint64_t t;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(t));
    return t;
}

/*
 * GIC 中「每顆 CPU 各自一份」的設定,每顆 CPU 都要在自己身上做一次:
 *   - CPU interface(GICC_*):這顆 CPU 接不接受中斷、優先權門檻
 *   - 中斷號 0~31 的啟用位元與優先權。這 32 個號碼是每顆 CPU 私有的
 *     (0~15 為 SGI,16~31 為 PPI,例如各自的計時器),同一個暫存器位址
 *     在不同的 CPU 上讀寫到的是不同的一份
 */
static void gic_cpu_init(void)
{
    *(volatile uint8_t *)(GICD_BASE + GICD_IPRIORITYR + IRQ_TIMER) = 0xA0;
    *(volatile uint8_t *)(GICD_BASE + GICD_IPRIORITYR + IRQ_IPI) = 0xA0;
    mmio_w32(GICD_BASE + GICD_ISENABLER0, (1u << IRQ_TIMER) | (1u << IRQ_IPI));
    mmio_w32(GICC_BASE + GICC_PMR, 0xFF);        /* 門檻最寬:接受所有優先權 */
    mmio_w32(GICC_BASE + GICC_CTLR, 1);
}

void arch_timer_init(unsigned hz)
{
    uint64_t freq;

    mmio_w32(GICD_BASE + GICD_CTLR, 1);          /* distributor 的總開關:全系統只有一個 */
    gic_cpu_init();

    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    interval = freq / hz;                        /* 每個 tick 的計數 */
    arch_timer_init_cpu();
}

/* 啟動「目前這顆 CPU」的計時器(generic timer 每顆 CPU 各有一個) */
void arch_timer_init_cpu(void)
{
    deadline = read_counter() + interval;
    __asm__ volatile("msr cntp_cval_el0, %0" : : "r"(deadline));
    __asm__ volatile("msr cntp_ctl_el0, %0" : : "r"(1UL));
}

/* AP 啟動後呼叫:讓這顆 CPU 可以接收中斷(含處理器間中斷) */
void arch_smp_cpu_init(void)
{
    gic_cpu_init();
}

/*
 * 發出處理器間中斷:寫入 GICD_SGIR。
 *   bit 23~16 目標 CPU 的位元遮罩(GIC 的 CPU 編號;QEMU virt 上等於 MPIDR 的最低 8 位元)
 *   bit 3~0   SGI 編號
 */
void arch_ipi_send(struct cpu *c)
{
    mmio_w32(GICD_BASE + GICD_SGIR, (1u << (16 + (unsigned)(c->arch_id & 7))) | IRQ_IPI);
}

/* 由 trap_handler 在收到 IRQ 時呼叫 */
void irq_handler(void)
{
    uint32_t iar = mmio_r32(GICC_BASE + GICC_IAR);   /* 認領中斷 */
    uint32_t id = iar & 0x3FF;

    if (id == IRQ_SPURIOUS)
        return;

    sched_irq_enter();                       /* 中斷中喚醒別人時,切換要留到返回前 */

    if (id == IRQ_IPI) {
        smp_ipi_handler();                   /* 別顆 CPU 的要求(見 kernel/smp.c) */
    } else if (id == IRQ_TIMER) {
        /*
         * 下一次的時間以「上一次的預定時間 + 間隔」計算,不以「現在」為基準,
         * 中斷送達的延遲才不會累積成時鐘變慢。若晚了不只一個間隔,
         * 把錯過的 tick 補記回來(說明見 arch/riscv64/timer.c)。
         */
        uint64_t now = read_counter();
        int n = 0;
        while (deadline <= now && n < MAX_CATCHUP) {
            timer_tick();
            deadline += interval;
            n++;
        }
        if (deadline <= now)                 /* 落後太多:放棄補記,重新對時 */
            deadline = now + interval;
        /* 寫入新的比較值:同時讓計時器的中斷訊號消失 */
        __asm__ volatile("msr cntp_cval_el0, %0" : : "r"(deadline));
    } else {
        kprintf("[irq] unexpected interrupt %u\n", id);
    }

    mmio_w32(GICC_BASE + GICC_EOIR, iar);            /* 處理完畢,GIC 才會再送下一個 */

    /* 必須在 EOI 之後:這裡可能切換到別的執行緒,很久才回來 */
    sched_irq_exit();
}
