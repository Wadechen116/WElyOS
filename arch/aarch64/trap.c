/*
 * trap.c (AArch64) - 例外處理
 */
#include <stddef.h>
#include <stdint.h>
#include "arch.h"
#include "kprintf.h"
#include "panic.h"
#include "proc.h"
#include "sched.h"
#include "trap.h"

/* 編譯期檢查:C 結構必須與 trap.S 的版面一致 */
_Static_assert(sizeof(struct trap_frame) == 38 * 8, "trap_frame size");
_Static_assert(offsetof(struct trap_frame, sp) == 31 * 8, "trap_frame sp");
_Static_assert(offsetof(struct trap_frame, elr) == 32 * 8, "trap_frame elr");

extern char trap_vectors[];

/* ESR_EL1 的 bit 31:26 為 EC(Exception Class),說明例外的類別 */
#define ESR_EC(esr)   (((esr) >> 26) & 0x3F)
#define EC_BRK        0x3C
#define EC_SVC        0x15            /* svc 指令:系統呼叫 */

static const char *ec_name(uint64_t ec)
{
    switch (ec) {
    case 0x00: return "Unknown reason (undefined instruction)";
    case 0x0E: return "Illegal execution state";
    case 0x15: return "SVC (system call)";
    case 0x20: return "Instruction abort (lower EL)";
    case 0x21: return "Instruction abort";
    case 0x22: return "PC alignment fault";
    case 0x24: return "Data abort (lower EL)";
    case 0x25: return "Data abort";
    case 0x26: return "SP alignment fault";
    case 0x3C: return "Breakpoint (BRK)";
    default:   return "Unknown";
    }
}

/* 向量種類:編號除以 4 的餘數 */
static const char *const kind_names[4] = { "sync", "IRQ", "FIQ", "SError" };

void trap_init(void)
{
    __asm__ volatile("msr vbar_el1, %0\n"
                     "isb"                       /* 確保設定在後續指令前生效 */
                     : : "r"((uintptr_t)trap_vectors));
}

static void dump_frame(const struct trap_frame *tf)
{
    kprintf("registers:\n");
    for (int i = 0; i < 31; i++) {
        kprintf("  x%d%s=0x%016llx%s", i, i < 10 ? " " : "",
                (unsigned long long)tf->x[i], (i % 3 == 2) ? "\n" : "");
    }
    kprintf("  sp =0x%016llx\n", (unsigned long long)tf->sp);
}

_Static_assert(sizeof(struct trap_frame) == 304, "trap_frame size (see trap.S, switch.S)");

size_t arch_frame_size(void)
{
    return sizeof(struct trap_frame);
}

void arch_frame_set_retval(void *tf, long v)
{
    ((struct trap_frame *)tf)->x[0] = (uint64_t)v;   /* 系統呼叫的回傳值放在 x0 */
}

uintptr_t arch_frame_user_sp(void *tf)
{
    return (uintptr_t)((struct trap_frame *)tf)->sp_el0;
}

unsigned arch_frame_call(void *vtf, uintptr_t frame, uintptr_t pc, uintptr_t arg, uintptr_t ret,
                         uintptr_t words[2])
{
    struct trap_frame *tf = vtf;

    (void)words;
    tf->sp_el0 = frame;                  /* 參數與返回位址都用暫存器傳:stack 上不必放東西 */
    tf->x[0] = arg;
    tf->x[30] = ret;
    tf->elr = pc;
    return 0;
}

void arch_frame_restore(void *vtf, const void *saved)
{
    struct trap_frame *tf = vtf;
    uint64_t spsr = tf->spsr, sp = tf->sp;   /* 例外等級與中斷遮蔽在 spsr;sp 是 kernel 自己的 stack */

    *tf = *(const struct trap_frame *)saved;
    tf->spsr = spsr;
    tf->sp = sp;
}

long arch_frame_get_retval(void *tf)
{
    return (long)((struct trap_frame *)tf)->x[0];
}

void arch_user_resume_asm(struct trap_frame *tf, uintptr_t ktop) __attribute__((noreturn));

void arch_user_resume(void *tf)
{
    uintptr_t lo, hi;

    arch_irq_disable();                  /* 還原暫存器到 eret 之間不可被中斷 */
    sched_stack_bounds(&lo, &hi);
    arch_user_resume_asm(tf, hi);
}

void trap_handler(struct trap_frame *tf, uint64_t kind)
{
    uint64_t ec = ESR_EC(tf->esr);

    /*
     * IRQ:非同步中斷。ELR 指向被打斷、尚未執行的指令,處理完直接返回即可。
     * 實際是哪個裝置要問 GIC(見 timer.c)。
     */
    /* 向量 8~11 = 例外來自較低的等級(EL0,使用者模式) */
    int from_user = (kind / 4) == 2;

    if (kind % 4 == 1) {
        irq_handler();
        if (from_user)
            user_return_check(tf);
        return;
    }

    /* FIQ / SError:未使用,收到就視為錯誤 */
    if (kind % 4 != 0)
        panic("unexpected %s (vector %llu)", kind_names[kind % 4],
              (unsigned long long)kind);

    /* 系統呼叫:編號在 x8,參數在 x0~x2,回傳值放回 x0。
       svc 的 ELR 已經指向下一條指令,不需要調整 */
    if (from_user && ec == EC_SVC) {
        tf->x[0] = (uint64_t)syscall_dispatch((long)tf->x[8], (long)tf->x[0],
                                              (long)tf->x[1], (long)tf->x[2], tf);
        user_return_check(tf);
        return;
    }

    /* 使用者程式造成的其他例外:先看是不是可以處理的 page fault(寫入時複製、stack 成長)。
       EC 0x24 = 資料存取、0x20 = 指令擷取;ESR 的 bit 6(WnR)為 1 表示是寫入。
       處理成功就返回,CPU 會重新執行同一條指令。否則只終止那個行程 */
    if (from_user) {
        if ((ec == 0x24 || ec == 0x20) &&
            proc_page_fault((uintptr_t)tf->far, ec == 0x24 && (tf->esr & (1u << 6))) == 0) {
            user_return_check(tf);
            return;
        }
        user_fault(ec_name(ec), tf->elr, tf->far);
    }

    /* 可恢復的例外:brk。ELR 指向 brk 本身,跳過它(固定 4 位元組)後繼續 */
    if (ec == EC_BRK) {
        kprintf("[trap] breakpoint at elr=0x%llx, resuming\n",
                (unsigned long long)tf->elr);
        tf->elr += 4;
        return;                                  /* trap.S 會 eret 回到 elr */
    }

    panic_begin();                               /* 致命錯誤:關中斷、讓 spinlock 放行 */
    sched_report_overflow(tf->sp, tf->far);      /* 若是 stack 溢位,先指出來 */
    kprintf("\n*** EXCEPTION: %s ***\n", ec_name(ec));
    kprintf("  esr =0x%llx   (EC=0x%llx)\n", (unsigned long long)tf->esr,
            (unsigned long long)ec);
    kprintf("  elr =0x%llx   (發生例外的指令位址)\n", (unsigned long long)tf->elr);
    kprintf("  far =0x%llx   (出錯的記憶體位址,僅 abort 類有效)\n",
            (unsigned long long)tf->far);
    kprintf("  spsr=0x%llx\n", (unsigned long long)tf->spsr);
    dump_frame(tf);
    arch_backtrace(tf->elr, tf->x[29]);          /* x29 = 出錯當下的 frame pointer */
    panic_halt();
}
