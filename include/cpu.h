#ifndef WELYOS_CPU_H
#define WELYOS_CPU_H
#include <stddef.h>
#include <stdint.h>

/*
 * 每顆 CPU 各自的狀態(per-CPU state)。
 *
 * 多核心之下,「目前正在執行的執行緒」「是否在中斷中」「需不需要重新排程」
 * 這類資訊每顆 CPU 都不一樣,不能再用單一個全域變數,所以集中放在這個結構,
 * 每顆 CPU 一份(cpus[] 陣列),各自只存取自己那一份。
 *
 * 取得「自己那一份」的方法依架構而定(見 arch/<名稱>/cpu_arch.h):
 *   RISC-V   tp 暫存器(kernel 執行期間固定指向自己的 struct cpu)
 *   AArch64  系統暫存器 TPIDR_EL1
 *   i386     由工作暫存器 TR(每顆 CPU 使用不同的 TSS)換算出編號
 *
 * 重要規則:this_cpu() 的結果只能在「中斷關閉」時使用。
 * 中斷開著的時候,執行緒隨時可能被搶佔,之後換到另一顆 CPU 上繼續執行,
 * 先前取得的指標就指向「別顆 CPU」的資料了。
 */
#define MAX_CPUS 8

struct thread;
struct vm_space;

struct cpu {
    /* ---- 前四個欄位由組合語言(例外入口)以固定的位移存取,順序不可更動 ---- */
    uintptr_t kstack_limit;      /* 目前執行緒 kernel stack 的最低可用位址;0 = 尚未啟用檢查 */
    uintptr_t kstack_top;        /* 目前執行緒 kernel stack 的頂端(從使用者模式進入時使用) */
    uintptr_t scratch[2];        /* 例外入口暫放暫存器的空間 */

    int id;                      /* CPU 編號(cpus[] 的索引) */
    int online;                  /* 已啟動並參與排程 */
    int in_irq;                  /* 正在中斷處理程式中 */
    volatile int need_resched;   /* 需要重新排程(可由別顆 CPU 設定) */
    int nlocks;                  /* 這顆 CPU 目前持有幾把 spinlock(檢查用) */
    struct thread *current;      /* 正在執行的執行緒;NULL = 排程器尚未啟動 */
    struct thread *idle;         /* 這顆 CPU 的 idle 執行緒 */
    struct thread *zombie;       /* 剛在這顆 CPU 上結束、等待回收的執行緒 */
    struct vm_space *loaded_space; /* 這顆 CPU 目前載入的位址空間;NULL = kernel 頁表 */
};

_Static_assert(offsetof(struct cpu, kstack_limit) == 0, "cpu.kstack_limit");
_Static_assert(offsetof(struct cpu, kstack_top) == sizeof(uintptr_t), "cpu.kstack_top");
_Static_assert(offsetof(struct cpu, scratch) == 2 * sizeof(uintptr_t), "cpu.scratch");

extern struct cpu cpus[MAX_CPUS];
extern int cpu_count;            /* 已啟動的 CPU 數(目前固定為 1) */

#include "cpu_arch.h"            /* arch_this_cpu()、arch_cpu_relax() */

static inline struct cpu *this_cpu(void) { return arch_this_cpu(); }
#endif
