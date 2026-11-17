#ifndef WELYOS_ARCH_H
#define WELYOS_ARCH_H
#include <stdint.h>

/*
 * 架構抽象介面。kernel/ 只透過這些函式與硬體互動,
 * 每個架構(arch/<名稱>/arch.c)各自實作。
 */

void arch_init(void);                          /* 初始化主控台,之後才能 kprintf */
void arch_putc(char c);                        /* 輸出一個字元到主控台 */
void arch_halt(void) __attribute__((noreturn));/* 關閉中斷並永久停住(panic 用) */
void arch_exit(int code) __attribute__((noreturn)); /* 結束模擬器(僅測試用) */
const char *arch_name(void);                   /* 架構名稱字串 */

/*
 * 中斷與計時器。
 *   arch_timer_init(hz): 設定計時器每秒產生 hz 次中斷(尚未開啟 CPU 中斷)。
 *                        每次中斷時,架構層須呼叫 timer_tick()(kernel/timer.c)。
 *   arch_irq_enable/disable: 開啟 / 關閉 CPU 接受中斷。
 *   arch_idle: 讓 CPU 休眠到下一次中斷再返回(須在中斷開啟時使用)。
 */
void arch_timer_init(unsigned hz);
void arch_irq_enable(void);
void arch_irq_disable(void);
void arch_idle(void);

/*
 * 暫時關閉中斷來保護一小段程式(臨界區段),結束後恢復「原本的」狀態:
 *     unsigned long flags = arch_irq_save();
 *     ... 不可被中斷打斷的操作 ...
 *     arch_irq_restore(flags);
 * 不直接用 disable/enable 的原因:如果進來之前中斷本來就是關的
 * (例如在中斷處理程式裡,或巢狀呼叫),結束時不可以擅自把它打開。
 */
unsigned long arch_irq_save(void);
void arch_irq_restore(unsigned long flags);

/*
 * 執行緒切換(實作在 arch/<名稱>/switch.S 與 arch.c)。
 *   arch_switch(old_sp, new_sp):把目前的 callee-saved 暫存器存到目前的 stack,
 *       stack pointer 寫入 *old_sp;再換成 new_sp 並還原那邊存的暫存器後返回。
 *   arch_stack_init(top, entry):在一塊新 stack(top 為最高位址)上擺好初始內容,
 *       使第一次 arch_switch 切過來時從 entry() 開始執行。回傳初始的 stack pointer。
 */
void arch_switch(void **old_sp, void *new_sp);
void *arch_stack_init(void *top, void (*entry)(void));

/*
 * 使用者模式。
 *   arch_set_kstack(top):告訴架構層「目前執行緒的 kernel stack 頂端」。從使用者模式
 *       進入 kernel(系統呼叫、中斷、例外)時,CPU 必須換到 kernel stack,
 *       不能繼續用使用者的 stack(那是不可信任的)。排程器每次切換時呼叫。
 *   arch_enter_user(entry, usp, arg):降到使用者模式,從 entry 開始執行,
 *       stack pointer 為 usp,並把 arg 當作第一個參數。不會返回;
 *       之後只會經由系統呼叫、中斷或例外回到 kernel。
 */
void arch_set_kstack(uintptr_t top);
void arch_enter_user(uintptr_t entry, uintptr_t usp, uintptr_t arg) __attribute__((noreturn));

/*
 * 多核心:啟動其他 CPU(實作在 arch/<名稱>/smp.c)。
 *   arch_smp_discover(bi, ids, max):找出「開機這顆以外」的 CPU,把它們硬體上的編號
 *       填入 ids(最多 max 個),回傳個數。同時完成啟動前的一次性準備。
 *   arch_smp_start(c):讓 c 代表的那顆 CPU 開始執行。呼叫前 c->id、c->arch_id、
 *       c->kstack_top 已設定好。那顆 CPU 會在自己的 stack 上、以 this_cpu() == c
 *       的狀態呼叫 smp_ap_main()。回傳 0 表示啟動要求已送出(不代表已經啟動)。
 *   arch_smp_done():所有 CPU 都啟動(或放棄)之後呼叫,收拾啟動用的暫時資源。
 *   arch_smp_cpu_init():每顆 AP 啟動後在自己身上呼叫:設定好接收處理器間中斷(IPI)。
 *   arch_timer_init_cpu():在目前這顆 AP 上啟動它自己的週期性計時器(頻率同開機的 CPU)。
 *   arch_ipi_send(c):對 c 發出處理器間中斷。c 收到後,架構層的中斷處理程式
 *       要呼叫 smp_ipi_handler()(kernel/smp.c)。
 *   arch_smp_park():讓目前這顆 CPU 永久休眠(關著中斷)。
 */
struct boot_info;
struct cpu;
int  arch_smp_discover(const struct boot_info *bi, uint64_t *ids, int max);
int  arch_smp_start(struct cpu *c);
void arch_smp_done(void);
void arch_smp_cpu_init(void);
void arch_timer_init_cpu(void);
void arch_ipi_send(struct cpu *c);
void arch_smp_park(void) __attribute__((noreturn));

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
