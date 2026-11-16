/*
 * trap.c (i386) - GDT、IDT 與例外處理
 */
#include <stddef.h>
#include <stdint.h>
#include "arch.h"
#include "kprintf.h"
#include "panic.h"
#include "trap.h"

_Static_assert(sizeof(struct trap_frame) == 13 * 4, "trap_frame size");
_Static_assert(offsetof(struct trap_frame, eip) == 10 * 4, "trap_frame eip");

/* ------------------------------------------------------------------ */
/* GDT(全域描述符表)                                                  */
/*                                                                    */
/* 保護模式下,cs/ds 等區段暫存器存的是「選擇子」,指向 GDT 的一項,      */
/* 該項描述區段的基底、長度與權限。multiboot 進入時區段是可用的,但      */
/* bootloader 的 GDT 位置不保證保留,而 IDT 需要參照程式碼區段選擇子,     */
/* 所以要建立自己的 GDT。這裡採「平坦模型」:基底 0、長度 4GB,等於       */
/* 不使用分段,位址保護之後交給分頁。                                    */
/* ------------------------------------------------------------------ */
struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;       /* 存在、權限等級、類型 */
    uint8_t  gran;         /* 高 4 位元:旗標;低 4 位元:limit 的高 4 位元 */
    uint8_t  base_high;
} __attribute__((packed));

/* lgdt / lidt 的運算元:表的長度減 1,以及表的位址 */
struct desc_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

#define KERNEL_CS 0x08     /* GDT 第 1 項:kernel 程式碼區段 */
#define KERNEL_DS 0x10     /* GDT 第 2 項:kernel 資料區段 */

static struct gdt_entry gdt[3];
static struct desc_ptr gdt_ptr;

extern void gdt_flush(const struct desc_ptr *p);

static void gdt_set(int i, uint8_t access)
{
    gdt[i].limit_low = 0xFFFF;
    gdt[i].base_low  = 0;
    gdt[i].base_mid  = 0;
    gdt[i].access    = access;
    gdt[i].gran      = 0xCF;   /* 4KB 粒度 + 32 位元區段;limit 高位 0xF -> 4GB */
    gdt[i].base_high = 0;
}

static void gdt_init(void)
{
    /* 第 0 項必須是全 0 的空描述符(static 已為 0) */
    gdt_set(1, 0x9A);          /* 存在、ring 0、程式碼、可執行可讀 */
    gdt_set(2, 0x92);          /* 存在、ring 0、資料、可讀寫 */
    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base  = (uint32_t)gdt;
    gdt_flush(&gdt_ptr);
}

/* ------------------------------------------------------------------ */
/* IDT(中斷描述符表):向量編號 -> 處理程式入口                           */
/* ------------------------------------------------------------------ */
struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;     /* 入口所在的程式碼區段選擇子 */
    uint8_t  zero;
    uint8_t  type_attr;    /* 0x8E = 存在、ring 0、32 位元中斷閘 */
    uint16_t offset_high;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct desc_ptr idt_ptr;

extern const uint32_t isr_table[32];     /* trap.S:32 個例外入口的位址 */

static void idt_init(void)
{
    /* 只安裝 CPU 例外(0~31)。32~255 留空;之後的步驟再安裝硬體中斷 */
    for (int i = 0; i < 32; i++) {
        idt[i].offset_low  = (uint16_t)(isr_table[i] & 0xFFFF);
        idt[i].selector    = KERNEL_CS;
        idt[i].zero        = 0;
        idt[i].type_attr   = 0x8E;
        idt[i].offset_high = (uint16_t)(isr_table[i] >> 16);
    }
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base  = (uint32_t)idt;
    __asm__ volatile("lidt %0" : : "m"(idt_ptr));
}

void trap_init(void)
{
    gdt_init();
    idt_init();
}

/* ------------------------------------------------------------------ */
/* 例外處理                                                            */
/* ------------------------------------------------------------------ */
#define VEC_BREAKPOINT 3
#define VEC_PAGE_FAULT 14

static const char *const exc_names[32] = {
    [0]  = "Divide error (#DE)",
    [1]  = "Debug (#DB)",
    [2]  = "Non-maskable interrupt",
    [3]  = "Breakpoint (#BP)",
    [4]  = "Overflow (#OF)",
    [5]  = "Bound range exceeded (#BR)",
    [6]  = "Invalid opcode (#UD)",
    [7]  = "Device not available (#NM)",
    [8]  = "Double fault (#DF)",
    [10] = "Invalid TSS (#TS)",
    [11] = "Segment not present (#NP)",
    [12] = "Stack-segment fault (#SS)",
    [13] = "General protection fault (#GP)",
    [14] = "Page fault (#PF)",
    [16] = "x87 floating-point (#MF)",
    [17] = "Alignment check (#AC)",
    [18] = "Machine check (#MC)",
    [19] = "SIMD floating-point (#XM)",
};

void trap_handler(struct trap_frame *tf)
{
    /*
     * 可恢復的例外:int3。它屬於「trap」類,CPU 存的 eip 已指向下一條指令,
     * 所以直接返回即可(「fault」類如 #UD 的 eip 則指向出錯指令本身)。
     */
    if (tf->vector == VEC_BREAKPOINT) {
        kprintf("[trap] breakpoint at eip=0x%x, resuming\n", tf->eip - 1);
        return;                                  /* trap.S 會 iret 回去 */
    }

    const char *name = "Unknown";
    if (tf->vector < 32 && exc_names[tf->vector])
        name = exc_names[tf->vector];

    kprintf("\n*** EXCEPTION: %s ***\n", name);
    kprintf("  vector=%u  err=0x%x\n", tf->vector, tf->err);
    kprintf("  eip   =0x%08x   (發生例外的指令位址)\n", tf->eip);
    kprintf("  cs    =0x%x  eflags=0x%08x\n", tf->cs, tf->eflags);
    if (tf->vector == VEC_PAGE_FAULT) {
        uint32_t cr2;                            /* CR2 存放造成 page fault 的位址 */
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        kprintf("  cr2   =0x%08x   (出錯的記憶體位址)\n", cr2);
    }
    kprintf("registers:\n");
    kprintf("  eax=0x%08x  ebx=0x%08x  ecx=0x%08x  edx=0x%08x\n",
            tf->eax, tf->ebx, tf->ecx, tf->edx);
    /* 同權限等級時 CPU 不推入 esp;出錯當下的 esp = eflags 之後的位址 */
    kprintf("  esi=0x%08x  edi=0x%08x  ebp=0x%08x  esp=0x%08x\n",
            tf->esi, tf->edi, tf->ebp, (uint32_t)(uintptr_t)(&tf->eflags + 1));
    arch_backtrace(tf->eip, tf->ebp);            /* ebp = 出錯當下的 frame pointer */
    panic_halt();
}
