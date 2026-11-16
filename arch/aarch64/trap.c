/*
 * trap.c (AArch64) - 例外處理
 */
#include <stddef.h>
#include <stdint.h>
#include "arch.h"
#include "kprintf.h"
#include "panic.h"
#include "trap.h"

/* 編譯期檢查:C 結構必須與 trap.S 的版面一致 */
_Static_assert(sizeof(struct trap_frame) == 36 * 8, "trap_frame size");
_Static_assert(offsetof(struct trap_frame, sp) == 31 * 8, "trap_frame sp");
_Static_assert(offsetof(struct trap_frame, elr) == 32 * 8, "trap_frame elr");

extern char trap_vectors[];

/* ESR_EL1 的 bit 31:26 為 EC(Exception Class),說明例外的類別 */
#define ESR_EC(esr)   (((esr) >> 26) & 0x3F)
#define EC_BRK        0x3C

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

void trap_handler(struct trap_frame *tf, uint64_t kind)
{
    uint64_t ec = ESR_EC(tf->esr);

    /*
     * IRQ:非同步中斷。ELR 指向被打斷、尚未執行的指令,處理完直接返回即可。
     * 實際是哪個裝置要問 GIC(見 timer.c)。
     */
    if (kind % 4 == 1) {
        irq_handler();
        return;
    }

    /* FIQ / SError:未使用,收到就視為錯誤 */
    if (kind % 4 != 0)
        panic("unexpected %s (vector %llu)", kind_names[kind % 4],
              (unsigned long long)kind);

    /* 可恢復的例外:brk。ELR 指向 brk 本身,跳過它(固定 4 位元組)後繼續 */
    if (ec == EC_BRK) {
        kprintf("[trap] breakpoint at elr=0x%llx, resuming\n",
                (unsigned long long)tf->elr);
        tf->elr += 4;
        return;                                  /* trap.S 會 eret 回到 elr */
    }

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
