/*
 * mmu.c (AArch64) - 頁表項目格式與 MMU 開關
 *
 * 項目格式(64 位元):
 *   bit 1:0   類型:0b11 = 中間層表示「下一層表格」,最後一層表示「4KB 頁」
 *                    0b01 = 中間層的「區塊」(block):直接對映一大塊(第 1 層為 2MB)
 *   bit 4:2   AttrIndx:記憶體屬性的編號,指向 MAIR_EL1 的第幾個位元組
 *   bit 7:6   AP:存取權限。0b00 = EL1 可讀寫,0b10 = EL1 唯讀
 *   bit 9:8   SH:共享屬性(0b11 = inner shareable,一般記憶體用)
 *   bit 10    AF:存取旗標。為 0 時第一次存取會觸發例外,所以直接設 1
 *   bit 12~47 實體頁的位址
 *   bit 53    PXN:EL1 不可執行
 *   bit 54    UXN:EL0 不可執行
 *
 * 和另外兩個架構不同的是「記憶體屬性」:ARM 要明確指出一段位址是
 * 一般記憶體(可以快取、可以合併或重排存取)還是裝置暫存器(每次讀寫都有
 * 副作用,必須照程式的順序、一次一個地執行)。屬性的定義放在 MAIR_EL1,
 * 每個項目用 AttrIndx 選用其中一個。
 *
 * 相關的系統暫存器:
 *   TTBR0_EL1  根表格的實體位址(低半部位址空間)
 *   TCR_EL1    轉換控制:位址寬度、頁大小、表格本身的快取屬性
 *   MAIR_EL1   8 個記憶體屬性定義
 *   SCTLR_EL1  bit 0 = M(開啟 MMU),bit 2 = C(資料快取),bit 12 = I(指令快取)
 */
#include "mmu.h"
#include "vm.h"

#define PTE_VALID     0x3UL                  /* 表格 / 頁 */
#define PTE_BLOCK     0x1UL                  /* 區塊(大分頁) */
#define PTE_ATTR(n)   ((uint64_t)(n) << 2)
#define PTE_AP_RO     (2UL << 6)
#define PTE_SH_INNER  (3UL << 8)
#define PTE_AF        (1UL << 10)
#define PTE_PXN       (1UL << 53)
#define PTE_UXN       (1UL << 54)
#define PTE_ADDR_MASK 0x0000FFFFFFFFF000UL

#define ATTR_NORMAL   0                      /* MAIR 位元組 0:一般記憶體 */
#define ATTR_DEVICE   1                      /* MAIR 位元組 1:裝置 */
/* 0xFF = 一般記憶體、可快取(write-back);0x00 = 裝置(nGnRnE,最嚴格) */
#define MAIR_VALUE    ((0xFFUL << (8 * ATTR_NORMAL)) | (0x00UL << (8 * ATTR_DEVICE)))

pte_t arch_pte_leaf(uintptr_t pa, unsigned flags, int level)
{
    pte_t e = (pte_t)pa | (level > 0 ? PTE_BLOCK : PTE_VALID) | PTE_AF | PTE_UXN;

    if (flags & VM_DEVICE)
        e |= PTE_ATTR(ATTR_DEVICE);
    else
        e |= PTE_ATTR(ATTR_NORMAL) | PTE_SH_INNER;
    if (!(flags & VM_WRITE))
        e |= PTE_AP_RO;
    if (!(flags & VM_EXEC))
        e |= PTE_PXN;
    return e;
}

pte_t arch_pte_table(uintptr_t table_pa)
{
    return (pte_t)table_pa | PTE_VALID;
}

int arch_pte_is_table(pte_t e, int level)
{
    (void)level;
    return (e & 3) == PTE_VALID;
}

int arch_pte_valid(pte_t e)
{
    return e & 1;
}

uintptr_t arch_pte_addr(pte_t e)
{
    return e & PTE_ADDR_MASK;
}

unsigned arch_pte_flags(pte_t e)
{
    unsigned f = VM_READ;
    if (!(e & PTE_AP_RO))
        f |= VM_WRITE;
    if (!(e & PTE_PXN))
        f |= VM_EXEC;
    if (((e >> 2) & 7) == ATTR_DEVICE)
        f |= VM_DEVICE;
    return f;
}

void arch_mmu_enable(pte_t *root)
{
    uint64_t mmfr0, tcr, sctlr;

    /* CPU 支援的實體位址寬度(PARange),要原樣填進 TCR 的 IPS 欄位 */
    __asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
    uint64_t ips = mmfr0 & 0xF;
    if (ips > 6)
        ips = 6;

    tcr = 25UL                 /* T0SZ = 25:虛擬位址寬度 64 - 25 = 39 位元 */
        | (1UL << 8)           /* IRGN0:走訪頁表時使用快取 */
        | (1UL << 10)          /* ORGN0:同上(外層快取) */
        | (3UL << 12)          /* SH0:inner shareable */
        | (0UL << 14)          /* TG0 = 0:4KB 頁 */
        | (1UL << 23)          /* EPD1:不使用 TTBR1(高半部位址空間) */
        | (ips << 32);         /* IPS:實體位址寬度 */

    __asm__ volatile("msr mair_el1, %0" : : "r"(MAIR_VALUE));
    __asm__ volatile("msr tcr_el1, %0" : : "r"(tcr));
    __asm__ volatile("msr ttbr0_el1, %0" : : "r"((uint64_t)(uintptr_t)root));
    __asm__ volatile("isb");                 /* 讓上面的設定對後續指令生效 */
    __asm__ volatile("tlbi vmalle1\n dsb nsh\n isb");   /* 清空 TLB */

    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1UL << 0) | (1UL << 2) | (1UL << 12);     /* M | C | I */
    __asm__ volatile("msr sctlr_el1, %0\n isb" : : "r"(sctlr));
}

void arch_tlb_flush(uintptr_t va)
{
    /* tlbi vaae1 的參數是頁號;dsb 等待失效完成,isb 讓後續指令看到結果 */
    __asm__ volatile("tlbi vaae1, %0\n dsb nsh\n isb" : : "r"((uint64_t)va >> 12));
}

void arch_mmu_map_devices(void)
{
    /* QEMU virt:GICv2(distributor 0x08000000、CPU interface 0x08010000,各 64KB)
       與 PL011 序列埠 */
    vm_map_range(0x08000000UL, 0x08000000UL, 0x20000, VM_READ | VM_WRITE | VM_DEVICE);
    vm_map(0x09000000UL, 0x09000000UL, VM_READ | VM_WRITE | VM_DEVICE);
}
