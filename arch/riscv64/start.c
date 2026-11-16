/*
 * start.c (RISC-V 64) - 由 boot.S 呼叫:初始化主控台,解析裝置樹,進入 kmain
 */
#include <stdint.h>
#include "arch.h"
#include "boot_info.h"
#include "fdt.h"
#include "kprintf.h"

/* hartid 由 OpenSBI 傳入 a0,dtb 為裝置樹位址(a1) */
void arch_start(uint64_t hartid, const void *dtb)
{
    static struct boot_info bi;
    (void)hartid;

    arch_init();
    if (fdt_parse(dtb, &bi) != 0)
        kprintf("warning: invalid device tree at %p\n", dtb);
    kmain(&bi);
}
