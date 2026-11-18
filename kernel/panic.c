/*
 * panic.c - 不可恢復錯誤的處理(與架構無關)
 */
#include <stdarg.h>
#include "arch.h"
#include "blackbox.h"
#include "kstring.h"
#include "cpu.h"
#include "kprintf.h"
#include "panic.h"
#include "smp.h"
#include "spinlock.h"

static int exit_on_panic;

void panic_set_exit(int on)
{
    exit_on_panic = on;
}

/*
 * 已經有一顆 CPU 在處理 panic 了嗎?(-1 = 沒有;否則是那顆 CPU 的編號)
 * 多核心下,第一顆 CPU panic 之後會讓所有的鎖失效以便印出訊息,其他 CPU 在被叫停之前
 * 可能因此看到不一致的狀態而跟著 panic。那些「第二個 panic」不是真正的問題,
 * 讓它們印出來只會和第一個的訊息混在一起(兩顆 CPU 同時輸出),把真正的原因蓋掉。
 * 所以只有第一顆 CPU 回報;之後才 panic 的別顆 CPU 直接停下。
 * 同一顆 CPU 再次 panic(印訊息的過程中又出錯)則直接結束,不再嘗試印任何東西。
 */
static volatile int panic_cpu = -1;

void panic_begin(void)
{
    arch_irq_disable();        /* 從此不再被搶佔 */
    int me = this_cpu()->id;
    int first = __sync_val_compare_and_swap(&panic_cpu, -1, me);
    if (first != -1) {
        if (first == me)
            panic_halt();      /* 處理 panic 的途中又出錯:放棄輸出,直接結束 */
        arch_halt();           /* 別顆 CPU 已經在回報了 */
    }
    spin_panic_mode();         /* 出事當下可能正持有某些鎖:之後一律放行,訊息才印得出來 */
    smp_stop_others();         /* 其他 CPU 不該在系統已經出錯的狀態下繼續執行 */
}

static int recorded;           /* 這次 panic 已經記進黑盒子了 */

void panic_halt(void)
{
    /* 由 CPU 例外直接走到這裡的(沒有經過 panic()):細節只印在主控台上,黑盒子裡留一筆概要 */
    if (!recorded) {
        recorded = 1;
        blackbox_panic(BB_PANIC, "fatal CPU exception in the kernel (details were printed on the console)");
    }
    if (exit_on_panic)
        arch_exit(1);          /* 非 0:讓腳本與 CI 能判斷失敗 */
    arch_halt();
}

void panic(const char *fmt, ...)
{
    va_list ap;

    static char text[BB_TEXT_MAX + 1];

    panic_begin();
    va_start(ap, fmt);
    kvsnprintf(text, sizeof(text), fmt, ap); /* 先組成文字:要印出來,也要記進黑盒子 */
    va_end(ap);
    kprintf("\n*** KERNEL PANIC: ");
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    arch_console_flush();
    va_end(ap);
    kprintf(" ***\n");
    arch_backtrace(0, arch_current_fp());
    /* 寫進黑盒子放在最後:萬一寫磁碟的過程又出錯,該印的訊息都已經印出來了 */
    if (!recorded) {
        recorded = 1;
        blackbox_panic(memcmp(text, "watchdog:", 9) == 0 ? BB_WATCHDOG : BB_PANIC, text);
    }
    panic_halt();
}
