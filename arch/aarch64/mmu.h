#ifndef WELYOS_AARCH64_MMU_H
#define WELYOS_AARCH64_MMU_H
#include <stdint.h>

/*
 * AArch64:4KB 頁、39 位元虛擬位址(TCR_EL1.T0SZ = 25),
 * 3 層(架構上稱為 level 1~3),每層 9 位元(512 項),項目為 64 位元。
 */
typedef uint64_t pte_t;
#define MMU_NAME    "AArch64 4K/39-bit"
#define MMU_LEVELS  3
#define MMU_BITS    9
#define MMU_VA_BITS 39
#define MMU_LARGE_LEVEL 1  /* 第 1 層的項目可以直接對映一大塊(2MB),不必再往下一層 */
#define MMU_HAS_NX  1

/* 產生「對映」項目。level 0 = 4KB 頁;level MMU_LARGE_LEVEL = 大分頁 */
pte_t     arch_pte_leaf(uintptr_t pa, unsigned flags, int level);
pte_t     arch_pte_table(uintptr_t table_pa);            /* 中間層:指向下一層表格 */
int       arch_pte_valid(pte_t e);
/* 第 level 層(> 0)的有效項目是「指向下一層表格」(1)還是「大分頁」(0) */
int       arch_pte_is_table(pte_t e, int level);
uintptr_t arch_pte_addr(pte_t e);                        /* PTE 中的實體位址 */
unsigned  arch_pte_flags(pte_t e);                       /* PTE 的權限 -> VM_* 旗標 */
void      arch_mmu_enable(pte_t *root);                  /* 載入根表格並開啟分頁 */
void      arch_tlb_flush(uintptr_t va);                  /* 讓一頁的 TLB 快取失效 */
void      arch_mmu_map_devices(void);                    /* 對映本架構的裝置 */
#endif
