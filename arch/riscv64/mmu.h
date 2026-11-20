#ifndef WELYOS_RISCV64_MMU_H
#define WELYOS_RISCV64_MMU_H
#include <stdint.h>

/*
 * RISC-V Sv39:39 位元虛擬位址,3 層,每層 9 位元(512 項),PTE 為 64 位元。
 */
typedef uint64_t pte_t;
#define MMU_NAME    "RISC-V Sv39"
/* 清除 TLB 的指令只作用在執行它的那顆 CPU;通知其他 CPU 要靠跨 CPU 呼叫(見 vm.c) */
#define MMU_TLB_BROADCAST 0
#define MMU_LEVELS  3
#define MMU_BITS    9
#define MMU_VA_BITS 38      /* Sv39 的低半部(bit 38 為 1 的是高半部,目前不使用) */
#define MMU_LARGE_LEVEL 1  /* 第 1 層的項目可以直接對映一大塊(2MB),不必再往下一層 */
#define MMU_HAS_NX  1
/*
 * ASID(address space identifier,位址空間編號):TLB 的每筆紀錄附帶一個編號,
 * 只有編號與目前相同的紀錄才會被使用。每個行程一個編號,切換行程時就
 * 不必清空 TLB,切回來時原本的紀錄還在。編號 1 ~ MMU_ASID_MAX-1 分配給行程,
 * 0 給 kernel 自己的頁表,MMU_ASID_MAX 是編號用完時大家共用的(切換時要清空)。
 */
#define MMU_ASID_MAX 255

/* 使用者位址範圍:4GB ~ 8GB(主記憶體與裝置都在 4GB 以下) */
#define USER_BASE   0x100000000UL
#define USER_TOP    0x200000000UL

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
void      arch_asid_flush(unsigned asid);                /* 讓某個位址空間編號的 TLB 紀錄全部失效 */
static inline void arch_mmu_init(void) {}                /* 本架構不需要事先偵測 */
static inline void arch_mmu_root_changed(pte_t *root) { (void)root; }
void      arch_tlb_flush(uintptr_t va);                  /* 讓一頁的 TLB 快取失效 */
void      arch_mmu_map_devices(void);                    /* 對映本架構的裝置 */
/* 頁表項目的單一寫入點:64 位元 CPU 的對齊 64 位元儲存本身不會撕裂;
   release 讓填好的下一層表格先於指向它的項目被看見(與 i386 的 cmpxchg8b 同一契約) */
static inline void arch_pte_set(volatile pte_t *e, pte_t v) { __atomic_store_n(e, v, __ATOMIC_RELEASE); }
static inline pte_t arch_pte_get(volatile pte_t *e) { return __atomic_load_n(e, __ATOMIC_ACQUIRE); }
#endif
