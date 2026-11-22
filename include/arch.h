#ifndef WELYOS_ARCH_H
#define WELYOS_ARCH_H
#include <stdint.h>

/*
 * 架構抽象介面。kernel/ 只透過這些函式與硬體互動,
 * 每個架構(arch/<名稱>/arch.c)各自實作。
 */

void arch_init(void);                          /* 初始化主控台,之後才能 kprintf */
void arch_putc(char c);                        /* 輸出一個字元到主控台 */
void arch_console_flush(void);                 /* 一次輸出(一個 kprintf)結束時呼叫:補做延後的工作,
                                                  例如 i386 把螢幕的游標移到新位置 */
int  arch_console_getc(void);                  /* 讀取一個輸入的字元;沒有輸入時立刻回傳 -1(不等待) */
void arch_console_irq_init(void);              /* 讓序列埠在收到字元時發出中斷,送到開機的那顆 CPU;
                                                  中斷處理程式要呼叫 console_rx()(kernel/console.c)。
                                                  要在 arch_timer_init(設定中斷控制器)之後呼叫 */
void arch_halt(void) __attribute__((noreturn));/* 關閉中斷並永久停住(panic 用) */
void arch_exit(int code) __attribute__((noreturn)); /* 結束模擬器(僅測試用) */

/*
 * arch_exit 結束前印出「exit: code N to the emulator」(docs/ISSUES.md N-002(a)):
 * 偶發的模擬器結束碼 5 再出現時,配合 win-exit.ps1 記下的 Windows 完整結束碼,分得出是 kernel 給錯了值
 * 還是模擬器自己當掉(第 51 步證實是後者:0xC0000005)。直接用 arch_putc 逐字輸出,
 * 不經過 kprintf(不取主控台的鎖,panic 時也能用,也不讓 panic 的呼叫環多一條路)。
 */
static inline void arch_exit_announce(int code)
{
    const char *s = "exit: code ";
    char d[12];
    unsigned u = code < 0 ? 0u - (unsigned)code : (unsigned)code, n = 0;
    while (*s)
        arch_putc(*s++);
    if (code < 0)
        arch_putc('-');
    do {
        d[n++] = (char)('0' + u % 10);
        u /= 10;
    } while (u);
    while (n)
        arch_putc(d[--n]);
    for (s = " to the emulator\n"; *s; s++)
        arch_putc(*s);
}
const char *arch_name(void);                   /* 架構名稱字串 */
uint64_t arch_rtc_read(void);                  /* 讀取硬體的即時時鐘(Real-Time Clock):現在是 1970-01-01 00:00:00 UTC
                                                  之後的第幾秒。沒有這種裝置回傳 0 */

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
/*
 * fork 用:tf 指向「這條執行緒從使用者模式進入 kernel 時存下來的暫存器」(trap frame)。
 * 它的格式各架構不同,所以 kernel 的其他部分只把它當成一塊不透明的資料:
 *   arch_frame_size()            這塊資料有多大
 *   arch_frame_set_retval(tf, v) 設定「系統呼叫的回傳值」那個暫存器
 *   arch_user_resume(tf)         依 tf 的內容還原所有暫存器並進入使用者模式,不會返回。
 *                                tf 必須位於目前執行緒的 kernel stack 上
 * 子行程就是這樣「從父行程呼叫 fork 的同一個地方」繼續執行的:
 * 複製一份父行程的 trap frame,把回傳值改成 0,然後 arch_user_resume。
 */
#include <stddef.h>
size_t arch_frame_size(void);
void arch_frame_set_retval(void *tf, long v);
void arch_user_resume(void *tf) __attribute__((noreturn));

/*
 * 訊號處理函式用。把「使用者模式被打斷時的暫存器」(tf)改成去執行另一個函式,之後再換回來:
 *   arch_frame_user_sp(tf)        目前的使用者 stack pointer
 *   arch_frame_call(tf, frame, pc, arg, ret, words)
 *                                 把 tf 改成:以 frame(已對齊 16)之下為 stack、呼叫 pc(arg)、
 *                                 返回到 ret。呼叫慣例需要先放在 stack 上的東西(i386 的返回位址
 *                                 與參數)填進 words[],回傳個數;它們要寫到新的 stack pointer 處
 *   arch_frame_restore(tf, saved) 以 saved(來自使用者的記憶體,不可信任)的內容取代 tf 的暫存器,
 *                                 但「只有 kernel 能決定的部分」(特權等級、中斷開關)保留 tf 原本的
 *   arch_frame_get_retval(tf)     讀出「系統呼叫回傳值」那個暫存器
 */
uintptr_t arch_frame_user_sp(void *tf);
unsigned  arch_frame_call(void *tf, uintptr_t frame, uintptr_t pc, uintptr_t arg, uintptr_t ret,
                          uintptr_t words[2]);
void      arch_frame_restore(void *tf, const void *saved);
long      arch_frame_get_retval(void *tf);

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

/*
 * 高精度時鐘(效能量測用):
 *   arch_clock_count() 一個單調遞增的 64 位元計數器(i386 TSC、RISC-V time CSR、AArch64 cntpct_el0),
 *                      從模擬器重置起算;所有 CPU 必須共用同一個時間基準(QEMU 上如此;
 *                      kernel/timer.c 以它推進系統時間,見 I-337)
 *   arch_clock_hz()    它每秒增加的次數;0 = 這台機器沒有可用的計數器
 */
uint64_t arch_clock_count(void);
/* 同上,但讀取指令與前後的記憶體存取依程式順序執行(I-342):i386 以 CPUID 夾住 RDTSC、
   aarch64 以 ISB 夾住 CNTPCT_EL0、riscv64 以 fence 夾住 time CSR。較慢,只給計數器同步檢查這類
   「讀數必須和前後的旗標交接有明確先後」的地方用,一般路徑仍用 arch_clock_count */
uint64_t arch_clock_count_ordered(void);
uint64_t arch_clock_hz(void);
/*
 * 一個 tick 相當於多少 arch_clock_count 的計數;0 = 沒有可用的共用計數器(例如 i386 沒有 TSC)。
 * kernel/timer.c 以它和共用計數器讓任何一顆 CPU 都能推進系統時間(I-337)。
 */
uint64_t arch_tick_interval(void);

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
