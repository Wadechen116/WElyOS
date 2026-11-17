#ifndef WELYOS_I386_TRAP_H
#define WELYOS_I386_TRAP_H
#include <stdint.h>

/* 與 trap.S 的 stack 版面一致(低位址在前) */
struct trap_frame {
    uint32_t edi, esi, ebp, esp_pusha, ebx, edx, ecx, eax;  /* pusha */
    uint32_t vector, err;                                    /* 入口推入 */
    uint32_t eip, cs, eflags;                                /* CPU 推入 */
};

void trap_init(void);                       /* 建立 GDT、TSS、IDT */
void trap_set_page_table(uint32_t cr3);     /* 分頁開啟時通知:TSS 需要記錄頁表位址 */
void trap_handler(struct trap_frame *tf);   /* 由 trap.S 呼叫 */
void irq_handler(uint32_t irq);             /* timer.c:處理硬體中斷 IRQ 0~15 */
#endif
