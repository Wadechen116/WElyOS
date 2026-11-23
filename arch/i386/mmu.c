/*
 * mmu.c (i386) - PAE 頁表項目格式、分頁開關與相關的保護功能
 *
 * 項目格式(64 位元):
 *   bit 0   P   存在(1 = 有效)
 *   bit 1   R/W 1 = 可寫
 *   bit 2   U/S 1 = 使用者模式(ring 3)可存取。x86 的規則是「每一層都要允許」
 *               才算允許,所以通往使用者頁的中間層表格也要設這個位元
 *   bit 7   PS  只用在第 1 層(PD):1 = 這一項直接對映 2MB 的大分頁
 *   bit 8   G   global:切換頁表(寫入 CR3)時「不」清掉這一筆 TLB 紀錄。
 *               用在 kernel 的頁:它們在每份頁表裡都相同,沒有理由每次切換都重查
 *   bit 12~ 實體頁的位址
 *   bit 63  NX  不可執行(需要 CPU 支援並設定 EFER.NXE)
 * 例外:第 2 層(PDPT)的項目只允許 P 位元,R/W、U/S 都是保留位元,必須為 0。
 *
 * 相關的控制暫存器:
 *   CR3           根表格(PDPT)的實體位址。PAE 模式下,寫入 CR3 時 CPU 會把
 *                 4 個 PDPT 項目讀進內部暫存器;之後修改 PDPT 必須重新寫入 CR3 才生效
 *   CR0 bit 31    PG:開啟分頁
 *   CR0 bit 16    WP:ring 0 也要遵守唯讀
 *   CR4 bit 5     PAE:使用 64 位元的項目格式
 *   CR4 bit 7     PGE:允許 global 頁
 *   CR4 bit 20    SMEP:禁止 ring 0「執行」使用者頁
 *   CR4 bit 21    SMAP:禁止 ring 0「讀寫」使用者頁(除非暫時以 stac 指令解除)
 *   EFER bit 11   NXE:啟用 NX 位元(EFER 是 MSR 0xC0000080)
 *   CR2           發生 page fault 時,CPU 把出錯的位址放在這裡
 *
 * SMEP / SMAP 的用意:kernel 若因為程式錯誤而解參考了使用者提供的指標,
 * 或跳到使用者控制的位址,硬體會直接擋下。這類錯誤是提權攻擊最常利用的途徑。
 */
#include "cpu.h"
#include "mmu.h"
#include "kprintf.h"
#include "panic.h"
#include "trap.h"
#include "vm.h"

#define PTE_P   0x001ULL
#define PTE_RW  0x002ULL
#define PTE_US  0x004ULL
#define PTE_PWT 0x008ULL                  /* write-through */
#define PTE_PCD 0x010ULL                  /* cache disable */
#define PTE_PS  0x080ULL
#define PTE_G   0x100ULL
#define PTE_SW_SHARED 0x200ULL            /* bit 9~11 保留給軟體使用,硬體忽略 */
#define PTE_SW_COW    0x400ULL
#define PTE_NX  (1ULL << 63)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

static int has_nx, has_pge, has_smep;
int smap_enabled;                        /* trap.S 的 arch_enter_user 會讀取 */

static void cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

int arch_mmu_has_nx(void)
{
    return has_nx;
}

void arch_mmu_init(void)
{
    uint32_t a, b, c, d, max_basic, max_ext;

    cpuid(0, 0, &max_basic, &b, &c, &d);
    cpuid(1, 0, &a, &b, &c, &d);
    if (!(d & (1u << 6)))
        panic("this CPU does not support PAE paging");
    if (!(d & (1u << 8)))                    /* arch_pte_set 的 64 位元一次寫入需要它 */
        panic("this CPU does not support CMPXCHG8B");
    has_pge = (d >> 13) & 1;
    int has_msr = (d >> 5) & 1;

    cpuid(0x80000000u, 0, &max_ext, &b, &c, &d);
    if (has_msr && max_ext >= 0x80000001u) {
        cpuid(0x80000001u, 0, &a, &b, &c, &d);
        has_nx = (d >> 20) & 1;
    }
    if (max_basic >= 7) {
        cpuid(7, 0, &a, &b, &c, &d);
        has_smep = (b >> 7) & 1;
        smap_enabled = (b >> 20) & 1;
    }

    /* NX 必須在建立任何帶 NX 位元的項目「並開啟分頁」之前啟用,
       否則 bit 63 會被當成保留位元而觸發 page fault */
    if (has_nx) {
        uint32_t lo, hi;
        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080u));
        lo |= 1u << 11;
        __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(0xC0000080u));
    }
}

pte_t arch_pte_leaf(uintptr_t pa, unsigned flags, int level)
{
    pte_t e = (pte_t)pa | PTE_P;

    if (flags & VM_WRITE)
        e |= PTE_RW;
    if (flags & VM_DEVICE)
        e |= PTE_PCD | PTE_PWT;              /* 裝置暫存器(LAPIC、HPET):不可快取,不靠 BIOS 的 MTRR(I-341)。
                                                PAT 維持開機預設時 PCD=1、PWT=1 即 UC */
    if (flags & VM_USER)
        e |= PTE_US;
    else if (has_pge)
        e |= PTE_G;                          /* kernel 的頁:切換頁表時保留 TLB 紀錄 */
    if (has_nx && !(flags & VM_EXEC))
        e |= PTE_NX;
    if (level > 0)
        e |= PTE_PS;
    if (!vm_sw_flags_ok(flags))
        panic("mmu: invalid software flags 0x%x", flags);
    unsigned sw = vm_sw_encode(flags);       /* 0~3,規則見 vm.h */
    if (sw & 1)
        e |= PTE_SW_SHARED;
    if (sw & 2)
        e |= PTE_SW_COW;
    return e;
}

pte_t arch_pte_table(uintptr_t table_pa, int user, int level)
{
    if (level == MMU_LEVELS - 1)             /* PDPT:只能有 P 位元 */
        return (pte_t)table_pa | PTE_P;
    /* 中間層設成可寫:實際權限取各層的交集,所以由最後一層決定 */
    return (pte_t)table_pa | PTE_P | PTE_RW | (user ? PTE_US : 0);
}

int arch_pte_valid(pte_t e)
{
    return (int)(e & PTE_P);
}

int arch_pte_is_table(pte_t e, int level)
{
    if (level == MMU_LEVELS - 1)             /* PDPT 的項目一定指向下一層 */
        return 1;
    return !(e & PTE_PS);
}

uintptr_t arch_pte_addr(pte_t e)
{
    return (uintptr_t)(e & PTE_ADDR_MASK);
}

unsigned arch_pte_flags(pte_t e)
{
    unsigned f = VM_READ;
    if (e & PTE_RW)
        f |= VM_WRITE;
    if (e & PTE_US)
        f |= VM_USER;
    if (e & PTE_PCD)
        f |= VM_DEVICE;
    if (!has_nx || !(e & PTE_NX))
        f |= VM_EXEC;                        /* 沒有 NX 的 CPU:有對映就可執行 */
    f |= vm_sw_decode(((e & PTE_SW_SHARED) ? 1u : 0u) | ((e & PTE_SW_COW) ? 2u : 0u));
    return f;
}

void arch_mmu_enable(pte_t *root)
{
    uint32_t cr0, cr4;

    trap_set_page_table((uint32_t)root);     /* double fault 的工作切換會載入它 */

    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= 1u << 5;                          /* PAE:必須在開啟分頁之前設定 */
    if (has_pge)
        cr4 |= 1u << 7;
    if (has_smep)
        cr4 |= 1u << 20;
    if (smap_enabled)
        cr4 |= 1u << 21;
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4));

    __asm__ volatile("mov %0, %%cr3" : : "r"((uint32_t)root));
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000u | 0x00010000u;        /* PG | WP */
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));

    if (this_cpu()->id != 0)                 /* 其他 CPU 啟動時也會執行這裡,不必重複印 */
        return;
    kprintf("vm: cpu features: NX %s, global pages %s, SMEP %s, SMAP %s\n",
            has_nx ? "on" : "unavailable", has_pge ? "on" : "unavailable",
            has_smep ? "on" : "unavailable", smap_enabled ? "on" : "unavailable");
}

void arch_mmu_switch(pte_t *root, unsigned asid, int flush)
{
    (void)asid;
    (void)flush;
    /* 寫入 CR3 會清掉所有「非 global」的 TLB 紀錄,也就是使用者的頁;
       kernel 的頁標了 G 位元,紀錄會保留下來 */
    __asm__ volatile("mov %0, %%cr3" : : "r"((uint32_t)root));
}

void arch_mmu_root_changed(pte_t *root)
{
    uint32_t cr3;

    /* PDPT 的項目在寫入 CR3 時才被讀進 CPU;若改的是正在使用的那一份,重新載入 */
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    if (cr3 == (uint32_t)root)
        __asm__ volatile("mov %0, %%cr3" : : "r"(cr3));
}

void arch_asid_flush(unsigned asid)
{
    (void)asid;                              /* 沒有 ASID;切換時已清除 */
}

void arch_tlb_flush(uintptr_t va)
{
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");   /* global 的紀錄也會被清掉 */
}

void arch_mmu_map_devices(void)
{
    /* VGA 文字模式的顯示記憶體 0xB8000~0xBFFFF。
       序列埠用的是 I/O port(in/out 指令),不經過分頁,不需要對映 */
    vm_map_range(0xB8000, 0xB8000, 0x8000, VM_READ | VM_WRITE);
}
