/*
 * main.c - kernel 的 C 語言進入點(與架構無關)
 *
 * 由各架構的 arch_start()(arch/<名稱>/start.c)在完成最基本的
 * 硬體初始化、並把開機資訊轉成 struct boot_info 後呼叫。
 */
#include <stdint.h>
#include "arch.h"
#include "boot_info.h"
#include "kprintf.h"
#include "panic.h"
#include "timer.h"

/*
 * 開機參數(cmdline)中是否含有指定的「完整單字」(以空白分隔)。
 * 例:cmdline = "kernel.elf autoexit",word = "autoexit" -> 1。
 * 整字比對可避免 "autoexit2" 誤判。
 */
static int cmdline_has(const char *cmd, const char *word)
{
    while (*cmd) {
        while (*cmd == ' ')                  /* 跳過前導空白 */
            cmd++;
        const char *a = cmd, *b = word;
        while (*a && *a != ' ' && *a == *b) {
            a++;
            b++;
        }
        /* 比對到 word 結尾,且 cmdline 該單字也剛好結束 -> 整字相符 */
        if ((*a == '\0' || *a == ' ') && *b == '\0')
            return 1;
        while (*cmd && *cmd != ' ')          /* 不符:跳到下一個單字 */
            cmd++;
    }
    return 0;
}

/*
 * 測試用:由開機參數 crash=<種類> 故意觸發錯誤,驗證例外與 panic 處理。
 *   crash=panic    直接呼叫 panic()
 *   crash=fault    觸發記憶體/保護錯誤(方式依架構而定,見 arch_test_fault)
 *   crash=illegal  執行非法指令
 *   crash=break    執行軟體中斷點指令(可恢復的例外,處理後會繼續往下)
 */
static void crash_test(const char *cmd)
{
    if (cmdline_has(cmd, "crash=panic")) {
        panic("test panic, value=%d", 42);
    } else if (cmdline_has(cmd, "crash=fault")) {
        kprintf("triggering fault...\n");
        arch_test_fault();
        kprintf("fault did not trap!\n");       /* 不應到達這裡 */
    } else if (cmdline_has(cmd, "crash=illegal")) {
        kprintf("executing illegal instruction...\n");
        arch_test_illegal();
    } else if (cmdline_has(cmd, "crash=break")) {
        kprintf("executing breakpoint instruction...\n");
        arch_test_breakpoint();
        kprintf("survived breakpoint\n");
    }
}

/* kernel 主函式。主控台此時已可使用。此函式不會返回。 */
void kmain(const struct boot_info *bi)
{
    kprintf("Hello WElyOS\n");
    kprintf("arch: %s (%u-bit)\n", arch_name(), (unsigned)(sizeof(void *) * 8));
    kprintf("cmdline: \"%s\"\n", bi->cmdline);
    kprintf("memory: base=0x%llx size=%llu KB\n",
            (unsigned long long)bi->mem_base,
            (unsigned long long)(bi->mem_size >> 10));
    kprintf("kmain=%p\n", (void *)kmain);

    /* autoexit:自動結束模擬器;並讓 panic 也結束模擬器(回傳失敗碼) */
    int autoexit = cmdline_has(bi->cmdline, "autoexit");
    panic_set_exit(autoexit);

    crash_test(bi->cmdline);

    /* 啟動計時器(100Hz)並開啟中斷,示範以 tick 計時 */
    timer_init(100);
    arch_irq_enable();
    kprintf("timer: %u Hz\n", timer_hz());
    for (unsigned sec = 1; sec <= 2; sec++) {
        while (timer_ticks() < (uint64_t)sec * timer_hz())
            arch_idle();                     /* 休眠到下一次中斷,不空轉 */
        kprintf("uptime: %u s (ticks=%llu)\n", sec,
                (unsigned long long)timer_ticks());
    }

    if (autoexit)
        arch_exit(0);
    for (;;)
        arch_idle();                         /* 閒置;中斷仍持續被處理 */
}
