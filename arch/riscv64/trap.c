/*
 * trap.c (RISC-V 64) - 例外處理
 */
#include <stddef.h>
#include <stdint.h>
#include "arch.h"
#include "kprintf.h"
#include "panic.h"
#include "trap.h"

/* 編譯期檢查:C 結構必須與 trap.S 的版面一致 */
_Static_assert(sizeof(struct trap_frame) == 35 * 8, "trap_frame size");
_Static_assert(offsetof(struct trap_frame, sepc) == 31 * 8, "trap_frame sepc");
_Static_assert(offsetof(struct trap_frame, stval) == 34 * 8, "trap_frame stval");

extern void trap_entry(void);

#define SCAUSE_INTERRUPT (1ULL << 63)   /* scause 最高位元:1 = 中斷,0 = 例外 */
#define EXC_BREAKPOINT   3
#define IRQ_S_TIMER      5              /* 中斷碼:S-mode 計時器 */

/* 例外原因表(RISC-V 特權規範,scause 低位元) */
static const char *const exc_names[] = {
    [0]  = "Instruction address misaligned",
    [1]  = "Instruction access fault",
    [2]  = "Illegal instruction",
    [3]  = "Breakpoint",
    [4]  = "Load address misaligned",
    [5]  = "Load access fault",
    [6]  = "Store/AMO address misaligned",
    [7]  = "Store/AMO access fault",
    [8]  = "Environment call from U-mode",
    [9]  = "Environment call from S-mode",
    [12] = "Instruction page fault",
    [13] = "Load page fault",
    [15] = "Store/AMO page fault",
};

void trap_init(void)
{
    /* 低 2 位元為模式:0 = direct,所有 trap 都跳到同一個位址 */
    __asm__ volatile("csrw stvec, %0" : : "r"((uintptr_t)trap_entry));
}

/* 暫存器名稱,順序與 struct trap_frame 前 31 個欄位相同 */
static const char *const reg_names[31] = {
    "ra ", "sp ", "gp ", "tp ", "t0 ", "t1 ", "t2 ", "s0 ", "s1 ",
    "a0 ", "a1 ", "a2 ", "a3 ", "a4 ", "a5 ", "a6 ", "a7 ",
    "s2 ", "s3 ", "s4 ", "s5 ", "s6 ", "s7 ", "s8 ", "s9 ", "s10", "s11",
    "t3 ", "t4 ", "t5 ", "t6 ",
};

static void dump_frame(const struct trap_frame *tf)
{
    const uint64_t *r = (const uint64_t *)tf;   /* 前 31 個欄位即 x1..x31 */

    kprintf("registers:\n");
    for (int i = 0; i < 31; i++) {
        kprintf("  %s=0x%016llx%s", reg_names[i], (unsigned long long)r[i],
                (i % 3 == 2 || i == 30) ? "\n" : "");
    }
}

void trap_handler(struct trap_frame *tf)
{
    uint64_t cause = tf->scause;

    if (cause & SCAUSE_INTERRUPT) {
        /*
         * 中斷是非同步的:它打斷的指令與中斷無關,sepc 指向「尚未執行的下一條
         * 指令」,處理完直接返回即可,不需要像例外那樣調整 sepc。
         */
        if ((cause & ~SCAUSE_INTERRUPT) == IRQ_S_TIMER) {
            timer_interrupt();
            return;
        }
        panic("unexpected interrupt, scause=0x%llx", (unsigned long long)cause);
    }

    /* 可恢復的例外:ebreak。印出訊息後跳過該指令,繼續執行 */
    if (cause == EXC_BREAKPOINT) {
        kprintf("[trap] breakpoint at sepc=0x%llx, resuming\n",
                (unsigned long long)tf->sepc);
        /* 指令長度:低 2 位元為 11 是 4 位元組指令,否則是 2 位元組壓縮指令 */
        uint16_t first = *(volatile uint16_t *)tf->sepc;
        tf->sepc += ((first & 3) == 3) ? 4 : 2;
        return;                                  /* trap.S 會 sret 回到 sepc */
    }

    /* 其餘例外:印出完整現場後 panic */
    const char *name = "Unknown";
    if (cause < sizeof(exc_names) / sizeof(exc_names[0]) && exc_names[cause])
        name = exc_names[cause];

    kprintf("\n*** EXCEPTION: %s ***\n", name);
    kprintf("  scause =0x%llx\n", (unsigned long long)cause);
    kprintf("  sepc   =0x%llx   (發生例外的指令位址)\n", (unsigned long long)tf->sepc);
    kprintf("  stval  =0x%llx   (相關位址或指令)\n", (unsigned long long)tf->stval);
    kprintf("  sstatus=0x%llx\n", (unsigned long long)tf->sstatus);
    dump_frame(tf);
    arch_backtrace(tf->sepc, tf->s0);            /* 從出錯當下的 frame pointer 往回追 */
    panic_halt();
}
