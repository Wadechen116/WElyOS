/*
 * arch.c (AArch64, QEMU virt) - 實作 include/arch.h
 *
 * 週邊以記憶體映射方式存取。序列埠為 ARM PL011。
 */
#include <stdint.h>
#include "arch.h"
#include "kprintf.h"

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
    for (;;)
        __asm__ volatile("wfe");           /* 等待事件;目前等同閒置 */
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

/* boot.S 定義的 stack 範圍,用來驗證 frame pointer 是否合理 */
extern char stack_bottom[], stack_top[];

/*
 * AArch64 的 frame record(使用 -fno-omit-frame-pointer,x29 為 frame pointer):
 *   *(fp)     = 上一層的 x29
 *   *(fp + 8) = 返回位址(x30,lr)
 * 每一步都檢查 fp 在 stack 內、16 位元組對齊、且往高位址前進。
 */
void arch_backtrace(uintptr_t pc, uintptr_t fp)
{
    int n = 0;

    kprintf("backtrace:\n");
    if (pc)
        kprintf("  #%d pc=%p\n", n++, (void *)pc);

    while (n < 16) {
        if (fp & 15 || fp < (uintptr_t)stack_bottom || fp + 16 > (uintptr_t)stack_top)
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
