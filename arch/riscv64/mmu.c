/*
 * mmu.c (RISC-V 64) - Sv39 頁表項目格式與分頁開關
 *
 * PTE 格式(64 位元):
 *   bit 0  V  有效
 *   bit 1  R  可讀      bit 2  W  可寫      bit 3  X  可執行
 *   bit 4  U  使用者模式(U-mode)可存取。注意 RISC-V 的 S-mode 預設「不能」
 *             存取 U 頁(除非設定 sstatus.SUM),也永遠不能執行 U 頁;
 *             kernel 讀寫使用者資料時改走恆等對映的實體位址
 *   bit 5  G  global:這筆對映在所有位址空間都相同,TLB 紀錄不分 ASID。用在 kernel 的頁
 *   bit 6  A  已被存取  bit 7  D  已被寫入
 *   bit 10~53 實體頁號(PPN = 實體位址 >> 12)
 *
 * RISC-V 用 R/W/X 區分兩種項目:三者全為 0 表示「指向下一層表格」,
 * 任一個為 1 表示「這就是最後的對映」。
 *
 * A、D 位元:規範允許硬體在第一次存取 / 寫入時自動設定,也允許硬體不設定、
 * 改成觸發 page fault 讓軟體來設。為了不依賴硬體的選擇,建立對映時直接設好。
 *
 * satp 暫存器:bit 63~60 模式(8 = Sv39,0 = 關閉分頁),bit 59~44 ASID,
 *              bit 43~0 根表格的頁號。
 */
#include "mmu.h"
#include "vm.h"

#define PTE_V  0x01UL
#define PTE_R  0x02UL
#define PTE_W  0x04UL
#define PTE_X  0x08UL
#define PTE_U  0x10UL
#define PTE_G  0x20UL
#define PTE_A  0x40UL
#define PTE_D  0x80UL
#define PTE_PPN_SHIFT 10
#define SATP_SV39 (8UL << 60)

/* RISC-V 各層的項目格式相同:在第 1 層放一個帶 R/W/X 的項目,就是 2MB 大分頁 */
static int asid_ok;                          /* 硬體實作的 ASID 位元數是否足夠 */

pte_t arch_pte_leaf(uintptr_t pa, unsigned flags, int level)
{
    (void)level;
    pte_t e = ((pte_t)pa >> 12 << PTE_PPN_SHIFT) | PTE_V | PTE_R | PTE_A | PTE_D;
    if (flags & VM_WRITE)
        e |= PTE_W;
    if (flags & VM_EXEC)
        e |= PTE_X;
    if (flags & VM_USER)
        e |= PTE_U;
    else
        e |= PTE_G;                          /* kernel 的頁:所有位址空間共用 TLB 紀錄 */
    return e;
}

pte_t arch_pte_table(uintptr_t table_pa, int user, int level)
{
    (void)user;                              /* 中間層不需要 U 位元,只看最後一層 */
    (void)level;
    return ((pte_t)table_pa >> 12 << PTE_PPN_SHIFT) | PTE_V;   /* R/W/X 全 0 */
}

int arch_pte_is_table(pte_t e, int level)
{
    (void)level;
    return !(e & (PTE_R | PTE_W | PTE_X));
}

int arch_pte_valid(pte_t e)
{
    return e & PTE_V;
}

uintptr_t arch_pte_addr(pte_t e)
{
    return (uintptr_t)((e >> PTE_PPN_SHIFT) & ((1UL << 44) - 1)) << 12;
}

unsigned arch_pte_flags(pte_t e)
{
    return VM_READ | ((e & PTE_W) ? VM_WRITE : 0) | ((e & PTE_X) ? VM_EXEC : 0) |
           ((e & PTE_U) ? VM_USER : 0);
}

void arch_mmu_enable(pte_t *root)
{
    uint64_t satp = SATP_SV39 | ((uint64_t)(uintptr_t)root >> 12);

    /* 偵測硬體實作了幾個 ASID 位元:規範允許少於 16 個,沒實作的位元寫入後讀回為 0。
       先把 ASID 欄位全寫 1 再讀回來看 */
    uint64_t probe = satp | (0xFFFFUL << 44), back;
    __asm__ volatile("csrw satp, %0" : : "r"(probe));
    __asm__ volatile("csrr %0, satp" : "=r"(back));
    asid_ok = ((back >> 44) & 0xFFFF) >= MMU_ASID_MAX;

    __asm__ volatile("csrw satp, %0" : : "r"(satp));
    __asm__ volatile("sfence.vma");          /* 清空 TLB:丟掉所有舊的轉換結果 */
}

void arch_mmu_switch(pte_t *root, unsigned asid, int flush)
{
    if (!asid_ok) {                          /* 不能用 ASID:退回每次切換都清空 */
        asid = 0;
        flush = 1;
    }
    uint64_t satp = SATP_SV39 | ((uint64_t)asid << 44) | ((uint64_t)(uintptr_t)root >> 12);

    __asm__ volatile("csrw satp, %0" : : "r"(satp));
    if (flush)
        __asm__ volatile("sfence.vma");      /* 舊位址空間的轉換結果全部作廢 */
}

void arch_asid_flush(unsigned asid)
{
    if (!asid_ok)
        return;
    /* sfence.vma rs1=x0, rs2=asid:清掉這個 ASID 的所有紀錄(global 的除外) */
    __asm__ volatile("sfence.vma zero, %0" : : "r"((uint64_t)asid));
}

void arch_tlb_flush(uintptr_t va)
{
    __asm__ volatile("sfence.vma %0, zero" : : "r"(va));
}

void arch_mmu_map_devices(void)
{
    /* QEMU virt:序列埠 NS16550 與用來結束模擬器的 test finisher。
       計時器透過 SBI 呼叫(ecall)操作,不是記憶體對映裝置 */
    vm_map(0x10000000UL, 0x10000000UL, VM_READ | VM_WRITE | VM_DEVICE);
    vm_map(0x00100000UL, 0x00100000UL, VM_READ | VM_WRITE | VM_DEVICE);
}
