/*
 * mmu.c (i386) - 頁表項目格式與分頁開關
 *
 * PTE 格式(32 位元):
 *   bit 0  P   存在(1 = 有效)
 *   bit 1  R/W 1 = 可寫
 *   bit 2  U/S 1 = 使用者模式可存取(目前都是 0,只有 kernel 可用)
 *   bit 7  PS  只用在第 1 層(page directory):1 = 這一項直接對映 4MB 的大分頁,
 *              不再指向下一層表格。需要先開啟 CR4 的 PSE 位元
 *   bit 12~31  實體頁的位址(低 12 位元必為 0,所以挪來放旗標)
 *
 * 相關的控制暫存器:
 *   CR3          根表格(page directory)的實體位址
 *   CR0 bit 31   PG:開啟分頁
 *   CR0 bit 16   WP:ring 0 也要遵守唯讀。不開的話 kernel 可以寫入任何頁,
 *                    把程式碼設成唯讀就沒有意義
 *   CR4 bit 4    PSE:允許 4MB 大分頁
 *   CR2          發生 page fault 時,CPU 把出錯的位址放在這裡
 */
#include "mmu.h"
#include "trap.h"
#include "vm.h"

#define PTE_P   0x001u
#define PTE_RW  0x002u
#define PTE_PS  0x080u
#define PTE_ADDR_MASK 0xFFFFF000u

pte_t arch_pte_leaf(uintptr_t pa, unsigned flags, int level)
{
    return (pte_t)pa | PTE_P | ((flags & VM_WRITE) ? PTE_RW : 0) |
           (level > 0 ? PTE_PS : 0);
}

int arch_pte_is_table(pte_t e, int level)
{
    (void)level;
    return !(e & PTE_PS);
}

/* 中間層設成可寫:實際權限取各層的交集,所以由最後一層決定 */
pte_t arch_pte_table(uintptr_t table_pa)
{
    return (pte_t)table_pa | PTE_P | PTE_RW;
}

int arch_pte_valid(pte_t e)
{
    return e & PTE_P;
}

uintptr_t arch_pte_addr(pte_t e)
{
    return e & PTE_ADDR_MASK;
}

unsigned arch_pte_flags(pte_t e)
{
    /* 沒有 NX 位元:只要有對映就可執行 */
    return VM_READ | VM_EXEC | ((e & PTE_RW) ? VM_WRITE : 0);
}

void arch_mmu_enable(pte_t *root)
{
    uint32_t cr0, cr4;

    trap_set_page_table((uint32_t)root);     /* double fault 的工作切換會載入它 */

    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= 0x10u;                            /* PSE:必須在開啟分頁之前設定 */
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));

    __asm__ volatile("mov %0, %%cr3" : : "r"((uint32_t)root));
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000u | 0x00010000u;        /* PG | WP */
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
}

void arch_tlb_flush(uintptr_t va)
{
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
}

void arch_mmu_map_devices(void)
{
    /* VGA 文字模式的顯示記憶體 0xB8000~0xBFFFF。
       序列埠用的是 I/O port(in/out 指令),不經過分頁,不需要對映 */
    vm_map_range(0xB8000, 0xB8000, 0x8000, VM_READ | VM_WRITE);
}
