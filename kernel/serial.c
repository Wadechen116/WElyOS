#include "io.h"
#include "serial.h"

#define COM1 0x3F8

void serial_init(void)
{
    outb(COM1 + 1, 0x00);   /* 關閉中斷 */
    outb(COM1 + 3, 0x80);   /* 開啟 DLAB 以設定鮑率 */
    outb(COM1 + 0, 0x01);   /* divisor = 1 -> 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8 data bits, no parity, 1 stop */
    outb(COM1 + 2, 0xC7);   /* 啟用並清空 FIFO */
    outb(COM1 + 4, 0x03);   /* DTR + RTS */
}

void serial_putc(char c)
{
    while (!(inb(COM1 + 5) & 0x20))   /* 等待傳送緩衝區空 */
        ;
    outb(COM1, (uint8_t)c);
}

void serial_puts(const char *s)
{
    while (*s)
        serial_putc(*s++);
}
