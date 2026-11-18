#ifndef WELYOS_RISCV64_TRAP_H
#define WELYOS_RISCV64_TRAP_H
#include <stdint.h>

/* 與 trap.S 儲存的格式一致:前 31 個欄位依序為 x1..x31 */
struct trap_frame {
    uint64_t ra, sp, gp, tp;
    uint64_t t0, t1, t2;
    uint64_t s0, s1;
    uint64_t a0, a1, a2, a3, a4, a5, a6, a7;
    uint64_t s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
    uint64_t t3, t4, t5, t6;
    uint64_t sepc, sstatus, scause, stval;
};

void trap_init(void);                       /* 設定 stvec,啟用例外處理 */
void trap_handler(struct trap_frame *tf);   /* 由 trap.S 呼叫 */
void timer_interrupt(void);                 /* timer.c:處理計時器中斷 */
void external_interrupt(void);              /* arch.c:處理 PLIC 送來的裝置中斷(序列埠) */
#endif
