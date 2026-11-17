/*
 * arch.c (AArch64, QEMU virt) - 實作 include/arch.h
 *
 * 週邊以記憶體映射方式存取。序列埠為 ARM PL011。
 */
#include <stdint.h>
#include "arch.h"
#include "kprintf.h"
#include "sched.h"

/* virt 機器的 PL011 UART,基底 0x09000000 */
#define UART0_BASE  0x09000000UL
#define UART_DR     (0x00 / 4)    /* 資料暫存器 */
#define UART_FR     (0x18 / 4)    /* 旗標暫存器:bit 5 = 傳送 FIFO 滿 */
#define UART_FR_TXFF (1u << 5)

static volatile uint32_t *const uart = (volatile uint32_t *)UART0_BASE;

/* QEMU 的 PL011 開機即可傳送,不需初始化(實機需設定鮑率與啟用,日後再補) */
void arch_init(void)
{
}

void arch_putc(char c)
{
    while (uart[UART_FR] & UART_FR_TXFF)   /* FIFO 滿就等待 */
        ;
    uart[UART_DR] = (uint32_t)c;
}

void arch_halt(void)
{
    arch_irq_disable();
    for (;;)
        __asm__ volatile("wfi");           /* 中斷已遮蔽,不會再被處理 */
}

/*
 * PSTATE 的 DAIF 四個遮蔽位元:D(除錯)、A(SError)、I(IRQ)、F(FIQ)。
 * daifclr / daifset 的立即值 #2 對應 I 位元:清除 = 允許 IRQ,設定 = 遮蔽 IRQ。
 */
void arch_irq_enable(void)
{
    __asm__ volatile("msr daifclr, #2");
}

void arch_irq_disable(void)
{
    __asm__ volatile("msr daifset, #2");
}

void arch_idle(void)
{
    __asm__ volatile("wfi");               /* 休眠到有中斷待處理 */
}

/*
 * 以 semihosting 的 SYS_EXIT(0x18)結束 QEMU 並回報結束碼。
 * 需要 QEMU 加 -semihosting;參數為 {原因, 子碼},子碼即結束碼。
 * 若未啟用 semihosting,hlt #0xf000 會變成例外(尚無處理程式)。
 */
static void semihosting_exit(int code)
{
    static volatile uint64_t block[2];
    register uint64_t x0 __asm__("x0") = 0x18;
    register uint64_t x1 __asm__("x1");

    block[0] = 0x20026;                  /* ADP_Stopped_ApplicationExit */
    block[1] = (uint64_t)code;
    x1 = (uint64_t)block;
    __asm__ volatile("hlt #0xf000" : "+r"(x0) : "r"(x1) : "memory");
}

/*
 * 結束 QEMU。成功(code == 0)以 PSCI 的 SYSTEM_OFF 關機:QEMU virt 以 hvc 為
 * 呼叫方式,功能碼 0x84000008 放在 x0,這是標準的關機介面,結束碼固定為 0。
 * PSCI 無法帶失敗碼,所以失敗時改用 semihosting。
 */
void arch_exit(int code)
{
    if (code != 0)
        semihosting_exit(code);
    register uint64_t x0 __asm__("x0") = 0x84000008UL;
    __asm__ volatile("hvc #0" : "+r"(x0) : : "memory");
    arch_halt();
}

const char *arch_name(void)
{
    return "aarch64";
}

unsigned long arch_irq_save(void)
{
    unsigned long flags;
    __asm__ volatile("mrs %0, daif" : "=r"(flags));   /* 讀出目前的遮蔽位元 */
    __asm__ volatile("msr daifset, #2");               /* 遮蔽 IRQ */
    return flags;
}

void arch_irq_restore(unsigned long flags)
{
    __asm__ volatile("msr daif, %0" : : "r"(flags));  /* 整組寫回原本的值 */
}

/*
 * 新執行緒的初始 stack,對應 switch.S 的版面(12 個 slot = 96 位元組):
 *     slot 0  = x19 = entry            :arch_thread_start 把 x30 清 0 後跳到 x19
 *     slot 10 = x29 = 0                :frame pointer 歸零,backtrace 以此判斷終點
 *     slot 11 = x30 = arch_thread_start:arch_switch 最後的 ret 會跳到這裡
 */
void arch_thread_start(void);            /* switch.S */

void *arch_stack_init(void *top, void (*entry)(void))
{
    uint64_t *sp = (uint64_t *)top - 12;
    for (int i = 0; i < 12; i++)
        sp[i] = 0;
    sp[0]  = (uint64_t)entry;
    sp[11] = (uint64_t)arch_thread_start;
    return sp;
}

/*
 * AArch64 的 frame record(使用 -fno-omit-frame-pointer,x29 為 frame pointer):
 *   *(fp)     = 上一層的 x29
 *   *(fp + 8) = 返回位址(x30,lr)
 * 每一步都檢查 fp 在 stack 內、16 位元組對齊、且往高位址前進。
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
        if (fp & 15 || fp < lo || fp + 16 > hi)
            break;
        uintptr_t prev = ((uintptr_t *)fp)[0];
        uintptr_t ra   = ((uintptr_t *)fp)[1];
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
    __asm__ volatile(".inst 0x00000000");    /* UDF #0:永久未定義的指令 */
}

void arch_test_breakpoint(void)
{
    __asm__ volatile("brk #0");
}

void arch_test_fault(void)
{
    /* 超出 CPU 實體位址範圍的位址;MMU 關閉時會產生 address size fault */
    volatile int *p = (volatile int *)0x0010000000000000UL;
    (void)*p;
}
