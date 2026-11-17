/*
 * start.c (RISC-V 64) - 由 boot.S 呼叫:初始化主控台,解析裝置樹,進入 kmain
 */
#include <stdint.h>
#include "arch.h"
#include "boot_info.h"
#include "cpu.h"
#include "fdt.h"
#include "kprintf.h"
#include "trap.h"

/* hartid 由 OpenSBI 傳入 a0,dtb 為裝置樹位址(a1) */
void arch_start(uint64_t hartid, const void *dtb)
{
    static struct boot_info bi;
    cpus[0].arch_id = hartid;            /* 開機的這顆 hart;之後啟動其他 hart 時要跳過它 */

    arch_init();
    trap_init();                         /* 之後的例外都會進入 trap_handler */
    if (fdt_parse(dtb, &bi) != 0)
        kprintf("warning: invalid device tree at %p\n", dtb);
    kmain(&bi);
}
