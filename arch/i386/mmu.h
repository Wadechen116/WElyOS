#ifndef WELYOS_I386_MMU_H
#define WELYOS_I386_MMU_H
#include <stdint.h>

/*
 * i386 的 PAE 分頁(Physical Address Extension):3 層,項目為 64 位元。
 *   第 2 層 PDPT :只有 4 項(虛擬位址最高 2 位元),每項涵蓋 1GB
 *   第 1 層 PD   :512 項,每項涵蓋 2MB(可直接當 2MB 大分頁)
 *   第 0 層 PT   :512 項,每項一個 4KB 頁
 * 和傳統的 2 層 32 位元格式相比,最重要的差別是項目有 64 位元,
 * 最高位元(bit 63)是 NX「不可執行」。傳統格式沒有這個位元,資料頁一律可執行。
 *
 * 共用的走訪程式把每層都當成 9 位元索引;PDPT 實際只用到前 4 項
 * (32 位元位址右移 30 位元後只會是 0~3),不影響正確性。
 */
typedef uint64_t pte_t;
#define MMU_NAME    "i386 PAE"
/* 清除 TLB 的指令只作用在執行它的那顆 CPU;通知其他 CPU 要靠跨 CPU 呼叫(見 vm.c) */
#define MMU_TLB_BROADCAST 0
#define MMU_LEVELS  3
#define MMU_BITS    9
#define MMU_VA_BITS 32
#define MMU_LARGE_LEVEL 1  /* 第 1 層的項目可以直接對映一大塊(2MB),不必再往下一層 */
#define MMU_HAS_NX  arch_mmu_has_nx()   /* 視 CPU 而定,開機時偵測 */
#define MMU_ASID_MAX 0     /* 32 位元模式沒有位址空間編號(PCID 只存在於 64 位元模式) */

/* 使用者位址範圍:1GB ~ 2GB(主記憶體與裝置都在更低的位址) */
#define USER_BASE   0x40000000u
#define USER_TOP    0x80000000u

int       arch_mmu_has_nx(void);
void      arch_mmu_init(void);                           /* 偵測 CPU 功能;建立頁表之前呼叫 */
/* 產生「對映」項目。level 0 = 4KB 頁;level MMU_LARGE_LEVEL = 大分頁 */
pte_t     arch_pte_leaf(uintptr_t pa, unsigned flags, int level);
/* 中間層:指向下一層表格。user 非 0 表示這張表格底下會放使用者頁;
   level 為「這個項目所在的層」 */
pte_t     arch_pte_table(uintptr_t table_pa, int user, int level);
int       arch_pte_valid(pte_t e);
/* 第 level 層(> 0)的有效項目是「指向下一層表格」(1)還是「大分頁」(0) */
int       arch_pte_is_table(pte_t e, int level);
uintptr_t arch_pte_addr(pte_t e);                        /* PTE 中的實體位址 */
unsigned  arch_pte_flags(pte_t e);                       /* PTE 的權限 -> VM_* 旗標 */
void      arch_mmu_enable(pte_t *root);                  /* 載入根表格並開啟分頁 */
/* 換用另一份頁表(行程切換)。asid 為位址空間編號;flush 非 0 表示要清空 TLB */
void      arch_mmu_switch(pte_t *root, unsigned asid, int flush);
void      arch_mmu_root_changed(pte_t *root);            /* 最上層表格新增了項目 */
void      arch_asid_flush(unsigned asid);                /* 讓某個位址空間編號的 TLB 紀錄全部失效 */
void      arch_tlb_flush(uintptr_t va);                  /* 讓一頁的 TLB 快取失效 */
void      arch_mmu_map_devices(void);                    /* 對映本架構的裝置 */
#endif
