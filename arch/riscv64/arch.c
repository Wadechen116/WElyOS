/*
 * arch.c (RISC-V 64, QEMU virt) - 實作 include/arch.h
 *
 * RISC-V 沒有 x86 那種獨立的 I/O port,週邊都映射在記憶體位址上,
 * 以一般的 load/store 存取(volatile 避免被編譯器優化掉)。
 */
#include <stdint.h>
#include "arch.h"

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
    for (;;)
        __asm__ volatile("wfi");         /* 等待中斷(目前中斷關閉,等同閒置) */
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
