/*
 * start.c (AArch64) - 由 boot.S 呼叫:初始化主控台,解析裝置樹,進入 kmain
 */
#include <stdint.h>
#include "arch.h"
#include "boot_info.h"
#include "fdt.h"
#include "kprintf.h"
#include "trap.h"

/* dtb 為 x0 傳入的裝置樹位址 */
void arch_start(const void *dtb)
{
    static struct boot_info bi;

    arch_init();
    trap_init();                         /* 之後的例外都會進入 trap_handler */
    if (fdt_parse(dtb, &bi) != 0)
        kprintf("warning: invalid device tree at %p\n", dtb);
    kmain(&bi);
}
