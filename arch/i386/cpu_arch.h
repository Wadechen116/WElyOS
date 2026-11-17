#ifndef WELYOS_CPU_ARCH_H
#define WELYOS_CPU_ARCH_H
#include <stdint.h>
/*
 * i386:沒有多餘的暫存器可以專門拿來指向 struct cpu。常見做法是用 fs/gs 區段,
 * 但那樣每次進出使用者模式都要保存、切換區段暫存器,容易出錯。
 *
 * 這裡改用工作暫存器 TR(task register):每顆 CPU 本來就必須有自己的 TSS
 * (其中記錄該 CPU 的 kernel stack),所以 TR 的值每顆 CPU 都不同,
 * 而且不受使用者模式影響。用 str 指令讀出 TR,即可換算出 CPU 編號。
 *
 * GDT 的配置(見 trap.c):
 *   第 3、4 項        第 0 顆 CPU 的 TSS(平常用的、double fault 用的)
 *   第 5、6 項        使用者模式的程式碼、資料區段
 *   第 7 項起每兩項   第 1 顆、第 2 顆…CPU 的 TSS
 * TR 尚未載入時(開機最早期)讀到 0,同樣換算成第 0 顆。
 */
/* 本檔由 cpu.h 在 struct cpu 與 cpus[] 宣告之後引入,所以這裡可以直接使用 */
static inline struct cpu *arch_this_cpu(void)
{
    uint32_t tr;
    __asm__("str %0" : "=r"(tr));
    unsigned i = (tr & 0xFFFF) >> 3;         /* 選擇子 -> GDT 的第幾項 */
    return &cpus[i < 7 ? 0 : ((i - 7) >> 1) + 1];
}

/* 自旋等待時的提示(pause):降低耗電,並避免離開迴圈時的管線清空 */
static inline void arch_cpu_relax(void)
{
    __asm__ volatile("pause");
}
#endif
