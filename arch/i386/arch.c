/*
 * arch.c (i386) - 實作 include/arch.h:序列埠 + VGA 雙輸出
 */
#include "arch.h"
#include "io.h"
#include "serial.h"
#include "vga.h"

void arch_init(void)
{
    serial_init();
    vga_init();
}

void arch_putc(char c)
{
    serial_putc(c);
    vga_putc(c);                         /* 同步輸出到螢幕 */
}

void arch_halt(void)
{
    for (;;)
        __asm__ volatile("cli; hlt");    /* 中斷關閉時 hlt 會永久停住 */
}

/*
 * 通知 QEMU 結束(需 -device isa-debug-exit,iobase=0xf4,iosize=0x04)。
 * 寫入 code 後,QEMU 以結束碼 (code << 1) | 1 離開,所以寫 0 會得到 1
 * (scripts/qemu.sh 會把 1 轉回 0)。實機上此 port 無作用,落入 halt。
 */
void arch_exit(int code)
{
    outb(0xF4, (uint8_t)code);
    arch_halt();
}

const char *arch_name(void)
{
    return "i386";
}
