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

/* QEMU isa-debug-exit:寫入 N,QEMU 以 (N<<1)|1 結束。僅用於測試。 */
static void qemu_exit(uint8_t code)
{
    outb(0xF4, code);
}

void kmain(void)
{
    serial_puts("Hello WElyOS\r\n");
    qemu_exit(0);          /* 結束碼 1 = 成功 */
}
