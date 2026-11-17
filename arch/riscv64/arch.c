/*
 * arch.c (RISC-V 64, QEMU virt) - 實作 include/arch.h
 *
 * RISC-V 沒有 x86 那種獨立的 I/O port,週邊都映射在記憶體位址上,
 * 以一般的 load/store 存取(volatile 避免被編譯器優化掉)。
 */
#include <stdint.h>
#include "arch.h"
#include "kprintf.h"
#include "sched.h"

/* virt 機器的 NS16550A 序列埠,基底 0x10000000 */
#define UART0_BASE  0x10000000UL
#define UART_THR    0    /* 傳送資料 */
#define UART_LSR    5    /* 線路狀態:bit 5 = 傳送緩衝區空 */

/* virt 機器的 SiFive test finisher:寫入特定值即可讓 QEMU 結束 */
#define FINISHER    0x100000UL
#define FINISHER_PASS 0x5555u    /* 成功,QEMU 以 0 結束 */
#define FINISHER_FAIL 0x3333u    /* 失敗,高 16 位元為結束碼 */

static volatile uint8_t *const uart = (volatile uint8_t *)UART0_BASE;

/* QEMU 的 UART 開機即可用,不需初始化(實機需設定鮑率,日後再補) */
void arch_init(void)
{
}

void arch_putc(char c)
{
    while (!(uart[UART_LSR] & 0x20))     /* 等待傳送緩衝區空 */
        ;
    uart[UART_THR] = (uint8_t)c;
}

void arch_halt(void)
{
    arch_irq_disable();
    for (;;)
        __asm__ volatile("wfi");         /* 中斷已關閉,不會再被處理 */
}

/* sstatus 的 bit 1(SIE):S-mode 全域中斷開關 */
void arch_irq_enable(void)
{
    __asm__ volatile("csrs sstatus, %0" : : "r"(2UL));
}

void arch_irq_disable(void)
{
    __asm__ volatile("csrc sstatus, %0" : : "r"(2UL));
}

void arch_idle(void)
{
    __asm__ volatile("wfi");             /* 休眠到有中斷待處理 */
}

void arch_exit(int code)
{
    volatile uint32_t *f = (volatile uint32_t *)FINISHER;
    *f = code ? (((uint32_t)code << 16) | FINISHER_FAIL) : FINISHER_PASS;
    arch_halt();                         /* 實機上無此裝置,落入閒置 */
}

const char *arch_name(void)
{
    return "riscv64";
}

unsigned long arch_irq_save(void)
{
    unsigned long old;
    /* csrrc:讀出 sstatus 舊值,同時清除指定位元(SIE),一條指令完成 */
    __asm__ volatile("csrrc %0, sstatus, %1" : "=r"(old) : "r"(2UL));
    return old;
}

void arch_irq_restore(unsigned long flags)
{
    if (flags & 2UL)                     /* 原本 SIE 是開的才打開 */
        __asm__ volatile("csrs sstatus, %0" : : "r"(2UL));
}

/*
 * 新執行緒的初始 stack,對應 switch.S 的版面(13 個 slot,補到 112 位元組):
 *     slot 0 = ra = arch_thread_start:arch_switch 最後的 ret 會跳到這裡
 *     slot 1 = s0 = 0                :frame pointer 歸零,backtrace 以此判斷終點
 *     slot 2 = s1 = entry            :arch_thread_start 把 ra 清 0 後跳到 s1
 */
void arch_thread_start(void);            /* switch.S */

void *arch_stack_init(void *top, void (*entry)(void))
{
    uint64_t *sp = (uint64_t *)top - 14;     /* 112 位元組 */
    for (int i = 0; i < 14; i++)
        sp[i] = 0;
    sp[0] = (uint64_t)arch_thread_start;
    sp[2] = (uint64_t)entry;
    return sp;
}

/*
 * RISC-V 的 frame 版面(使用 -fno-omit-frame-pointer):s0 = fp 指向 frame 頂端,
 *   *(fp - 8)  = 返回位址 ra
 *   *(fp - 16) = 上一層的 fp
 * 沿著這條鏈往回走。每一步都檢查 fp 在 stack 內、對齊、且往高位址前進,
 * 避免遇到損壞的鏈而再次出錯。
 */
void arch_backtrace(uintptr_t pc, uintptr_t fp)
{
    int n = 0;
    uintptr_t lo, hi;

    kprintf("backtrace:\n");
    if (pc)
        kprintf("  #%d pc=%p\n", n++, (void *)pc);

    sched_stack_bounds(&lo, &hi);        /* 目前執行緒的 stack 範圍 */
    while (n < 16) {
        if (fp & 7 || fp < lo + 16 || fp > hi)
            break;
        uintptr_t ra   = ((uintptr_t *)fp)[-1];
        uintptr_t prev = ((uintptr_t *)fp)[-2];
        if (!ra)
            break;
        kprintf("  #%d ra=%p\n", n++, (void *)ra);
        if (prev <= fp)
            break;
        fp = prev;
    }
}

void arch_test_illegal(void)
{
    __asm__ volatile(".4byte 0xffffffff");   /* 全 1 不是合法指令編碼 */
}

void arch_test_breakpoint(void)
{
    __asm__ volatile("ebreak");
}

void arch_test_fault(void)
{
    volatile int *p = (volatile int *)0;     /* virt 機器的位址 0 沒有裝置 */
    (void)*p;
}
