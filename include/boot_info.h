#ifndef WELYOS_BOOT_INFO_H
#define WELYOS_BOOT_INFO_H
#include <stdint.h>

/*
 * 與架構無關的開機資訊。各架構的 start.c 負責從自己的來源
 * (i386: multiboot info;RISC-V/AArch64: 裝置樹 DTB)轉成此結構。
 */
#define BOOT_MAX_RESERVED 8
#define BOOT_MAX_CPUS     8

/* 一段實體記憶體範圍:[base, base + size) */
struct mem_region {
    uint64_t base;
    uint64_t size;
};

struct boot_info {
    const char *cmdline;   /* 開機參數,保證非 NULL(可為空字串) */
    uint64_t mem_base;     /* 主記憶體起始實體位址 */
    uint64_t mem_size;     /* 主記憶體大小(位元組);0 表示未知 */

    /*
     * 主記憶體中「已經有東西、不可配置」的範圍,由 bootloader 留下:
     * 例如裝置樹(DTB)本身、multiboot 的開機參數字串。
     * kernel 映像不列在這裡,實體記憶體管理員會用 linker 符號自行排除。
     */
    struct mem_region reserved[BOOT_MAX_RESERVED];
    unsigned n_reserved;
    /*
     * 裝置樹列出的 CPU(RISC-V 的 hartid、AArch64 的 MPIDR 親和值),含開機的那一顆。
     * i386 沒有裝置樹,CPU 清單另外從 ACPI 取得,這裡為 0。
     */
    uint64_t cpu_ids[BOOT_MAX_CPUS];
    unsigned ncpus;
};

/* 登記一段保留範圍;size 為 0 或表格已滿時忽略 */
static inline void boot_reserve(struct boot_info *bi, uint64_t base, uint64_t size)
{
    if (size == 0 || bi->n_reserved >= BOOT_MAX_RESERVED)
        return;
    bi->reserved[bi->n_reserved].base = base;
    bi->reserved[bi->n_reserved].size = size;
    bi->n_reserved++;
}

/* kernel 主函式,由各架構的 arch_start() 呼叫,不會返回 */
void kmain(const struct boot_info *bi) __attribute__((noreturn));
#endif
