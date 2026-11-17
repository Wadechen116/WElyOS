#ifndef WELYOS_I386_TRAP_H
#define WELYOS_I386_TRAP_H
#include <stdint.h>

/* 與 trap.S 的 stack 版面一致(低位址在前) */
struct trap_frame {
    uint32_t es, ds;                                         /* isr_common 推入 */
    uint32_t edi, esi, ebp, esp_pusha, ebx, edx, ecx, eax;  /* pusha */
    uint32_t vector, err;                                    /* 入口推入 */
    uint32_t eip, cs, eflags;                                /* CPU 推入 */
    uint32_t user_esp, user_ss;      /* CPU 推入;僅在從使用者模式進來時存在 */
};

void trap_init(void);                       /* 建立 GDT、TSS、IDT */
void trap_init_ap(int id);                  /* 其他 CPU 啟動時:載入 GDT、IDT 與自己的 TSS */
void trap_set_page_table(uint32_t cr3);     /* 分頁開啟時通知:TSS 需要記錄頁表位址 */
void trap_handler(struct trap_frame *tf);   /* 由 trap.S 呼叫 */
void irq_handler(uint32_t irq);             /* timer.c:處理硬體中斷 IRQ 0~15 */

/* LAPIC 使用的中斷向量(多核心) */
#define VEC_LAPIC_TIMER 48                  /* 每顆 CPU 自己的計時器 */
#define VEC_IPI         49                  /* 處理器間中斷 */
#define VEC_SPURIOUS    255                 /* 假中斷 */
void lapic_interrupt(uint32_t vector);      /* smp.c:處理上面兩種 LAPIC 中斷 */
uint64_t arch_tsc_per_tick(void);           /* timer.c:一個 tick 相當於多少 TSC 計數;0 = 沒有 TSC */
#endif
