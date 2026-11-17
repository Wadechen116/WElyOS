#ifndef WELYOS_SMP_H
#define WELYOS_SMP_H
#include "boot_info.h"

/*
 * 多核心(Symmetric Multiprocessing,SMP)的啟動。
 *
 * 開機時只有一顆 CPU 在執行(BSP,Bootstrap Processor);其他的(AP,Application
 * Processor)停在韌體裡,要由 kernel 逐一叫醒。smp_init() 負責:
 *   1. 向架構層詢問有哪些 CPU
 *   2. 替每一顆準備 struct cpu 與自己的 stack
 *   3. 逐一啟動,等它回報
 * 目前(第二段)AP 啟動後只做自我檢查,然後待命;讓它們參與排程是下一段的工作。
 *
 * run_test 非 0 時,所有 CPU 一起執行一輪並行測試(見 smp.c)。
 * 必須在排程器啟動、開中斷之後呼叫(等待時要靠計時器判斷逾時)。
 */
void smp_init(const struct boot_info *bi, int run_test);

/* 已啟動的 CPU 數(含開機的那一顆) */
int smp_started(void);

/* AP 啟動後由架構層呼叫(此時已在自己的 stack 上、分頁已開、this_cpu() 可用)。不會返回 */
void smp_ap_main(void) __attribute__((noreturn));
#endif
