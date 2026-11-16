#include <stdint.h>

#define COM1 0x3F8

static inline void outb(uint16_t port, uint8_t v)
{
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static void serial_putc(char c)
{
    while (!(inb(COM1 + 5) & 0x20))   /* 等待傳送緩衝區空 */
        ;
    outb(COM1, (uint8_t)c);
}

static void serial_puts(const char *s)
{
    while (*s)
        serial_putc(*s++);
}

void kmain(void)
{
    serial_puts("Hello WElyOS\r\n");
}
