/*
 * start.c (i386) - 由 boot.S 呼叫:初始化主控台、把 multiboot info
 * 轉成 boot_info,然後進入 kmain。
 */
#include <stdint.h>
#include "arch.h"
#include "boot_info.h"
#include "multiboot.h"
#include "kprintf.h"
#include "trap.h"

/*
 * boot.S 依 multiboot 規範傳入:
 *   magic    = 0x2BADB002,確認是由 multiboot bootloader 載入
 *   mbi_addr = multiboot info 結構位址
 */
void arch_start(uint32_t magic, uint32_t mbi_addr)
{
    static struct boot_info bi;

    arch_init();                         /* 先初始化輸出裝置,才能印訊息 */
    trap_init();                         /* 建立 GDT、IDT;之後的例外都會進入 trap_handler */

    /* 魔術數不符代表不是由 multiboot 載入,info 結構不可信,直接停機 */
    if (magic != MULTIBOOT_BOOTLOADER_MAGIC) {
        kprintf("bad multiboot magic: %x\n", magic);
        arch_halt();
    }

    const struct multiboot_info *mbi = (const struct multiboot_info *)mbi_addr;
    /* flags 的 bit 2 為 1 才代表 cmdline 欄位有效 */
    bi.cmdline = (mbi->flags & (1u << 2)) ? (const char *)mbi->cmdline : "";
    /* flags 的 bit 0 為 1 才代表 mem_* 有效;mem_upper 為 1MB 以上的 KB 數 */
    bi.mem_base = 0x100000;
    bi.mem_size = (mbi->flags & 1u) ? (uint64_t)mbi->mem_upper * 1024 : 0;

    kmain(&bi);
}
