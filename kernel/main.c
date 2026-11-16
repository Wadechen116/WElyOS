#include <stdint.h>
#include "io.h"
#include "kprintf.h"
#include "serial.h"

/* QEMU isa-debug-exit:寫入 N,QEMU 以 (N<<1)|1 結束。僅用於測試。 */
static void qemu_exit(uint8_t code)
{
    outb(0xF4, code);
}

void kmain(void)
{
    serial_init();
    kprintf("Hello WElyOS\n");
    kprintf("char=%c str=%s\n", 'A', "text");
    kprintf("dec=%d neg=%d uns=%u\n", 42, -42, 4000000000u);
    kprintf("hex=%x HEX=%X pad=[%08x] w=[%5d] z=[%05d]\n",
            0xBEEF, 0xBEEF, 0x1234, 7, -7);
    kprintf("ptr=%p kmain=%p pct=100%%\n", (void *)0, (void *)kmain);
    qemu_exit(0);
}
