/*
 * trap.c (i386) - GDT、IDT 與例外處理
 */
#include <stddef.h>
#include <stdint.h>
#include "arch.h"
#include "cpu.h"
#include "kprintf.h"
#include "panic.h"
#include "proc.h"
#include "sched.h"
#include "trap.h"

_Static_assert(sizeof(struct trap_frame) == 17 * 4, "trap_frame size");
_Static_assert(offsetof(struct trap_frame, eip) == 12 * 4, "trap_frame eip");

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

#define TSS_MAIN  0x18     /* GDT 第 3 項:平常使用的 TSS */
#define TSS_DF    0x20     /* GDT 第 4 項:double fault 專用的 TSS */

#define USER_CS   0x2B     /* GDT 第 5 項 | RPL 3:使用者程式碼區段 */
#define USER_DS   0x33     /* GDT 第 6 項 | RPL 3:使用者資料區段 */

/* 第 0~6 項如上;第 7 項起每兩項留給第 1 顆、第 2 顆…CPU 的 TSS(見 cpu_arch.h)。
   每顆 CPU 要有自己的 TSS:裡面的 esp0 是「這顆 CPU 上正在執行的執行緒」的 kernel stack */
#define GDT_ENTRIES (7 + 2 * (MAX_CPUS - 1))
#define AP_TSS_INDEX(id) (7 + 2 * ((id) - 1))        /* 第 id 顆 CPU(id >= 1)的 TSS 在第幾項 */
static struct gdt_entry gdt[GDT_ENTRIES];
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

/* ------------------------------------------------------------------ */
/* TSS 與 double fault                                                  */
/*                                                                    */
/* 問題:x86 發生例外時,CPU「自己」會先把 eflags、cs、eip 推上目前的     */
/* stack,然後才執行我們的入口。如果例外的原因是 kernel stack 溢位,      */
/* esp 正指著沒有對映的保護頁,CPU 這個推入的動作本身就會再出錯,         */
/* 於是升級成 double fault(#DF);#DF 同樣要推入 stack,再失敗就是         */
/* triple fault,CPU 直接重置,螢幕上什麼訊息都沒有。                    */
/* RISC-V 與 AArch64 的硬體不會自動用到 stack,所以在入口檢查就夠了;    */
/* x86 必須在「CPU 推入之前」就換好 stack,只能靠硬體機制。             */
/*                                                                    */
/* 解法:把 #DF 的 IDT 項目設成「工作閘」(task gate)。觸發時 CPU 不推入  */
/* 目前的 stack,而是做一次硬體工作切換:把當下所有暫存器存進目前的 TSS,  */
/* 再從另一個 TSS 載入整組暫存器,包含全新的 esp 與 eip。               */
/* TSS(task state segment)就是存放這一整組暫存器的結構。              */
/* ------------------------------------------------------------------ */
struct tss {
    uint32_t link;                   /* 切換前的 TSS 選擇子(CPU 填入) */
    uint32_t esp0, ss0;              /* 由使用者模式進入 kernel 時使用的 stack(之後才用到) */
    uint32_t esp1, ss1, esp2, ss2;
    uint32_t cr3;
    uint32_t eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap, iomap_base;
} __attribute__((packed));

_Static_assert(sizeof(struct tss) == 104, "tss size");

static struct tss main_tss;          /* 第 0 顆 CPU 平常的 TSS:#DF 時 CPU 把出錯當下的狀態存在這裡 */
static struct tss ap_tss[MAX_CPUS - 1];  /* 其他 CPU 平常的 TSS */
static struct tss df_tss;            /* #DF 時載入的狀態 */
static uint8_t df_stack[8192] __attribute__((aligned(16)));

static void double_fault_handler(void) __attribute__((noreturn));

static void gdt_set_tss(int i, const struct tss *t)
{
    uint32_t base = (uint32_t)t, limit = sizeof(*t) - 1;

    gdt[i].limit_low = (uint16_t)limit;
    gdt[i].base_low  = (uint16_t)(base & 0xFFFF);
    gdt[i].base_mid  = (uint8_t)((base >> 16) & 0xFF);
    gdt[i].access    = 0x89;         /* 存在、ring 0、32 位元 TSS(未使用中) */
    gdt[i].gran      = 0x00;         /* 位元組粒度 */
    gdt[i].base_high = (uint8_t)(base >> 24);
}

static void tss_init(void)
{
    main_tss.ss0 = KERNEL_DS;
    main_tss.iomap_base = sizeof(struct tss);

    /* #DF 發生時 CPU 會把這些值載入對應的暫存器 */
    df_tss.eip = (uint32_t)double_fault_handler;
    df_tss.esp = (uint32_t)(df_stack + sizeof(df_stack));
    df_tss.eflags = 0x2;             /* bit 1 恆為 1;IF = 0,中斷關閉 */
    df_tss.cs = KERNEL_CS;
    df_tss.ss = df_tss.ds = df_tss.es = df_tss.fs = df_tss.gs = KERNEL_DS;
    df_tss.iomap_base = sizeof(struct tss);
    /* df_tss.cr3 要等分頁開啟後才知道,由 trap_set_page_table() 設定 */

    gdt_set_tss(3, &main_tss);
    gdt_set_tss(4, &df_tss);
    for (int id = 1; id < MAX_CPUS; id++) {
        ap_tss[id - 1].ss0 = KERNEL_DS;
        ap_tss[id - 1].iomap_base = sizeof(struct tss);
        gdt_set_tss(AP_TSS_INDEX(id), &ap_tss[id - 1]);
    }
}

/* 目前這顆 CPU 平常使用的 TSS */
static struct tss *cpu_tss(void)
{
    int id = this_cpu()->id;
    return id == 0 ? &main_tss : &ap_tss[id - 1];
}

/* 由使用者模式進入 kernel 時,CPU 從 TSS 的 esp0 取得 kernel stack 的位置 */
void arch_set_kstack(uintptr_t top)
{
    cpu_tss()->esp0 = (uint32_t)top;
}

/* 工作切換時 CPU 也會載入 TSS 裡的 CR3;必須是有效的頁表,否則立刻 triple fault */
void trap_set_page_table(uint32_t cr3)
{
    main_tss.cr3 = cr3;
    df_tss.cr3 = cr3;
    for (int i = 0; i < MAX_CPUS - 1; i++)
        ap_tss[i].cr3 = cr3;
}

static void gdt_init(void)
{
    /* 第 0 項必須是全 0 的空描述符(static 已為 0) */
    gdt_set(1, 0x9A);          /* 存在、ring 0、程式碼、可執行可讀 */
    gdt_set(2, 0x92);          /* 存在、ring 0、資料、可讀寫 */
    tss_init();                /* 填入第 3、4 項(TSS) */
    /* 使用者模式的區段:同樣是平坦的 0~4GB,差別只在權限等級(DPL)為 3。
       記憶體的保護不靠區段,而是靠分頁的 U/S 位元 */
    gdt_set(5, 0xFA);          /* 存在、ring 3、程式碼、可執行可讀 */
    gdt_set(6, 0xF2);          /* 存在、ring 3、資料、可讀寫 */
    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base  = (uint32_t)gdt;
    gdt_flush(&gdt_ptr);

    /* ltr:告訴 CPU 目前使用哪一個 TSS。之後發生工作切換時,
       CPU 會把當下的暫存器存進這個 TSS */
    __asm__ volatile("ltr %%ax" : : "a"((uint16_t)TSS_MAIN));
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

#define NUM_VECTORS 48                   /* 0~31 CPU 例外 + 32~47 硬體中斷 */
extern const uint32_t isr_table[NUM_VECTORS];   /* trap.S:各入口的位址 */

static void idt_init(void)
{
    /* 安裝 CPU 例外(0~31)與硬體中斷(32~47)。其餘留空 */
    for (int i = 0; i < NUM_VECTORS; i++) {
        idt[i].offset_low  = (uint16_t)(isr_table[i] & 0xFFFF);
        idt[i].selector    = KERNEL_CS;
        idt[i].zero        = 0;
        idt[i].type_attr   = 0x8E;
        idt[i].offset_high = (uint16_t)(isr_table[i] >> 16);
    }
    /* 向量 8(double fault)改為工作閘:offset 不使用,selector 指向 df_tss,
       類型 0x85 = 存在、ring 0、task gate */
    idt[8].offset_low  = 0;
    idt[8].selector    = TSS_DF;
    idt[8].zero        = 0;
    idt[8].type_attr   = 0x85;
    idt[8].offset_high = 0;

    /* 向量 0x80(系統呼叫):類型 0xEE = 存在、DPL 3、32 位元中斷閘。
       DPL 3 表示允許 ring 3 用 int 指令觸發;其他向量的 DPL 是 0,
       使用者程式若執行 int $14 之類想偽造例外,CPU 會改發 #GP */
    extern void isr128(void);
    idt[0x80].offset_low  = (uint16_t)((uint32_t)isr128 & 0xFFFF);
    idt[0x80].selector    = KERNEL_CS;
    idt[0x80].zero        = 0;
    idt[0x80].type_attr   = 0xEE;
    idt[0x80].offset_high = (uint16_t)((uint32_t)isr128 >> 16);

    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base  = (uint32_t)idt;
    __asm__ volatile("lidt %0" : : "m"(idt_ptr));
}

void trap_init(void)
{
    gdt_init();
    idt_init();
}

/* 其他 CPU 啟動時呼叫:GDT 與 IDT 的內容全系統共用,但「載入」是每顆 CPU 各自的事,
   TR 則要指向自己的 TSS。這一步之後 this_cpu() 才會回傳正確的結果 */
void trap_init_ap(int id)
{
    gdt_flush(&gdt_ptr);
    __asm__ volatile("ltr %%ax" : : "a"((uint16_t)(AP_TSS_INDEX(id) * 8)));
    __asm__ volatile("lidt %0" : : "m"(idt_ptr));
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

/*
 * double fault 的處理:此時已在 df_stack 上,中斷關閉。
 * 出錯當下的 eip、esp、ebp 由 CPU 存進了「出錯的那顆 CPU 平常使用的 TSS」;
 * 是哪一個,看 df_tss.link:工作切換時 CPU 會把切換前的 TSS 選擇子記在這裡。
 * 不會返回。
 * (限制:#DF 用的 TSS 與 stack 全系統只有一份,兩顆 CPU 同時發生時會互相干擾)
 */
static void double_fault_handler(void)
{
    uint32_t cr2;
    unsigned idx = (df_tss.link & 0xFFFF) >> 3;
    struct tss *t = idx < 7 ? &main_tss : &ap_tss[(idx - 7) >> 1];

    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    panic_begin();                               /* 致命錯誤:關中斷、讓 spinlock 放行 */
    if (!sched_report_overflow(t->esp, cr2))
        kprintf("\n(double fault not caused by a stack overflow)\n");
    kprintf("\n*** EXCEPTION: Double fault (#DF) ***\n");
    kprintf("  eip   =0x%08x   (發生例外的指令位址)\n", t->eip);
    kprintf("  esp   =0x%08x  ebp=0x%08x\n", t->esp, t->ebp);
    kprintf("  cr2   =0x%08x   (出錯的記憶體位址)\n", cr2);
    arch_backtrace(t->eip, t->ebp);
    panic_halt();
}

void trap_handler(struct trap_frame *tf)
{
    /*
     * 硬體中斷(向量 32~47 = IRQ 0~15):非同步,eip 指向尚未執行的指令,
     * 處理完直接返回。中斷閘進入時 CPU 已自動關閉中斷(清 IF),
     * iret 還原 eflags 時會重新開啟。
     */
    int from_user = (tf->cs & 3) == 3;       /* cs 的低 2 位元 = 被打斷時的權限等級 */

    if (tf->vector >= 32 && tf->vector < 48) {
        irq_handler(tf->vector - 32);
        if (from_user)
            user_return_check();
        return;
    }

    /* 系統呼叫:編號在 eax,參數在 ebx、ecx、edx,回傳值放回 eax */
    if (tf->vector == 0x80) {
        tf->eax = (uint32_t)syscall_dispatch((long)tf->eax, (long)tf->ebx,
                                             (long)tf->ecx, (long)tf->edx);
        user_return_check();
        return;
    }

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

    /* 使用者程式造成的例外:只終止那個行程,kernel 繼續運作 */
    if (from_user) {
        uint32_t addr = 0;
        if (tf->vector == VEC_PAGE_FAULT)
            __asm__ volatile("mov %%cr2, %0" : "=r"(addr));
        user_fault(name, tf->eip, addr);
    }

    panic_begin();                               /* 致命錯誤:關中斷、讓 spinlock 放行 */
    if (tf->vector == VEC_PAGE_FAULT) {
        /* esp 還在有效範圍、但存取落到保護頁的情況(例如超大的區域陣列) */
        uint32_t fault;
        __asm__ volatile("mov %%cr2, %0" : "=r"(fault));
        sched_report_overflow((uint32_t)(uintptr_t)(&tf->eflags + 1), fault);
    }
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
