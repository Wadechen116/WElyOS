/*
 * arch.c (AArch64, QEMU virt) - 實作 include/arch.h
 *
 * 週邊以記憶體映射方式存取。序列埠為 ARM PL011。
 */
#include <stdint.h>
#include "arch.h"

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
 * 以 PSCI(電源管理介面)的 SYSTEM_OFF 關機:QEMU virt 以 hvc 為呼叫方式,
 * 功能碼 0x84000008 放在 x0。QEMU 會直接結束(結束碼固定 0,忽略 code)。
 */
void arch_exit(int code)
{
    (void)code;
    register uint64_t x0 __asm__("x0") = 0x84000008UL;
    __asm__ volatile("hvc #0" : "+r"(x0) : : "memory");
    arch_halt();
}

const char *arch_name(void)
{
    return "aarch64";
}
