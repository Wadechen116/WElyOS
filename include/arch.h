#ifndef WELYOS_ARCH_H
#define WELYOS_ARCH_H
#include <stdint.h>

/*
 * 架構抽象介面。kernel/ 只透過這些函式與硬體互動,
 * 每個架構(arch/<名稱>/arch.c)各自實作。
 */

void arch_init(void);                          /* 初始化主控台,之後才能 kprintf */
void arch_putc(char c);                        /* 輸出一個字元到主控台 */
void arch_halt(void) __attribute__((noreturn));/* 讓 CPU 永久閒置 */
void arch_exit(int code) __attribute__((noreturn)); /* 結束模擬器(僅測試用) */
const char *arch_name(void);                   /* 架構名稱字串 */

/*
 * 沿著 frame pointer 鏈印出呼叫堆疊(需以 -fno-omit-frame-pointer 編譯)。
 *   pc: 發生例外的位址,非 0 時當作第 0 層印出;傳 0 則略過
 *   fp: 起始的 frame pointer
 * 印出的是位址,可用 addr2line 還原成函式與行號(見 docs/DEVLOG.md)。
 */
void arch_backtrace(uintptr_t pc, uintptr_t fp);

/* 目前函式的 frame pointer */
#define arch_current_fp() ((uintptr_t)__builtin_frame_address(0))

/* 測試用:執行一個非法指令,觸發「非法指令」例外 */
void arch_test_illegal(void);

/*
 * 測試用:觸發一個記憶體/保護類的錯誤。各架構能用的方式不同:
 *   RISC-V  讀取位址 0(無對應裝置 -> access fault)
 *   AArch64 讀取超出實體位址範圍的位址(-> data abort)
 *   i386    載入不存在的區段選擇子(-> #GP;未開分頁時讀任何位址都不會出錯)
 */
void arch_test_fault(void);

/* 測試用:執行軟體中斷點指令(RISC-V ebreak、x86 int3、AArch64 brk) */
void arch_test_breakpoint(void);
#endif
