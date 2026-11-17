/*
 * panic.c - 不可恢復錯誤的處理(與架構無關)
 */
#include <stdarg.h>
#include "arch.h"
#include "kprintf.h"
#include "panic.h"

static int exit_on_panic;

void panic_set_exit(int on)
{
    exit_on_panic = on;
}

void panic_halt(void)
{
    if (exit_on_panic)
        arch_exit(1);          /* 非 0:讓腳本與 CI 能判斷失敗 */
    arch_halt();
}

void panic(const char *fmt, ...)
{
    va_list ap;

    arch_irq_disable();        /* 從此不再被搶佔,其他執行緒也不會繼續跑 */
    kprintf("\n*** KERNEL PANIC: ");
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    kprintf(" ***\n");
    arch_backtrace(0, arch_current_fp());
    panic_halt();
}
