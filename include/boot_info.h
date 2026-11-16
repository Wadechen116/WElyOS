#ifndef WELYOS_BOOT_INFO_H
#define WELYOS_BOOT_INFO_H
#include <stdint.h>

/*
 * 與架構無關的開機資訊。各架構的 start.c 負責從自己的來源
 * (i386: multiboot info;RISC-V/AArch64: 裝置樹 DTB)轉成此結構。
 */
struct boot_info {
    const char *cmdline;   /* 開機參數,保證非 NULL(可為空字串) */
    uint64_t mem_base;     /* 主記憶體起始實體位址 */
    uint64_t mem_size;     /* 主記憶體大小(位元組);0 表示未知 */
};

/* kernel 主函式,由各架構的 arch_start() 呼叫,不會返回 */
void kmain(const struct boot_info *bi) __attribute__((noreturn));
#endif
