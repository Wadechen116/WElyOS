#ifndef WELYOS_MULTIBOOT_H
#define WELYOS_MULTIBOOT_H
#include <stdint.h>

/* bootloader 進入 kernel 時放在 eax 的魔術數,用來確認是 multiboot 載入 */
#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002u

/*
 * multiboot v1 info 結構(規範定義的前幾個欄位,僅宣告目前用到的部分)。
 * 位址由 bootloader 放在 ebx。各欄位是否有效由 flags 的對應位元決定。
 */
struct multiboot_info {
    uint32_t flags;          /* bit 0: mem_* 有效;bit 2: cmdline 有效 */
    uint32_t mem_lower;      /* 低於 1MB 的記憶體,單位 KB(通常 639) */
    uint32_t mem_upper;      /* 1MB 以上的連續記憶體,單位 KB */
    uint32_t boot_device;
    uint32_t cmdline;        /* 開機參數字串(以 NUL 結尾)的實體位址 */
};
#endif
