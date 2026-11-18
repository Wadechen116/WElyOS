#ifndef WELYOS_AARCH64_TRAP_H
#define WELYOS_AARCH64_TRAP_H
#include <stdint.h>

/* 與 trap.S 儲存的格式一致 */
struct trap_frame {
    uint64_t x[31];        /* x0..x30(x29 = frame pointer,x30 = 返回位址 lr) */
    uint64_t sp;           /* 被打斷時的 sp */
    uint64_t elr;          /* 返回位址(發生例外的指令) */
    uint64_t spsr;         /* 被打斷時的處理器狀態 */
    uint64_t esr;          /* 例外原因 */
    uint64_t far;          /* 出錯的記憶體位址(僅部分例外有效) */
    uint64_t sp_el0;       /* 使用者模式的 stack pointer */
    uint64_t pad;          /* 讓整個 frame 維持 16 位元組對齊 */
};

void trap_init(void);                                    /* 設定 VBAR_EL1 */
void trap_handler(struct trap_frame *tf, uint64_t kind); /* 由 trap.S 呼叫 */
void irq_handler(void);                                  /* timer.c:處理 IRQ */
void uart_rx_irq_enable(void);                           /* arch.c:PL011 收到字元時發中斷 */
void uart_rx_irq_ack(void);                              /* arch.c:清除 PL011 的接收中斷狀態 */
#endif
