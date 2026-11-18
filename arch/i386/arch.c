/*
 * arch.c (i386) - 實作 include/arch.h:序列埠 + VGA 雙輸出
 */
#include "arch.h"
#include "io.h"
#include "kprintf.h"
#include "sched.h"
#include "serial.h"
#include "vga.h"

void arch_init(void)
{
    serial_init();
    vga_init();
}

void arch_putc(char c)
{
    serial_putc(c);
    vga_putc(c);                         /* 同步輸出到螢幕 */
}

/* 一次輸出結束:更新螢幕上的硬體游標(見 vga.c) */
void arch_console_flush(void)
{
    vga_sync();
}

/* 輸入只接序列埠(QEMU 把它接到終端機)。PS/2 鍵盤尚未支援 */
int arch_console_getc(void)
{
    return serial_getc();
}

void arch_halt(void)
{
    for (;;)
        __asm__ volatile("cli; hlt");    /* 中斷關閉時 hlt 會永久停住 */
}

/* eflags 的 IF 位元:sti 設定(接受中斷),cli 清除 */
void arch_irq_enable(void)
{
    __asm__ volatile("sti");
}

void arch_irq_disable(void)
{
    __asm__ volatile("cli");
}

void arch_idle(void)
{
    __asm__ volatile("hlt");             /* 休眠到下一次中斷 */
}

/*
 * 通知 QEMU 結束(需 -device isa-debug-exit,iobase=0xf4,iosize=0x04)。
 * 寫入 code 後,QEMU 以結束碼 (code << 1) | 1 離開,所以寫 0 會得到 1
 * (scripts/qemu.sh 會把 1 轉回 0)。實機上此 port 無作用,落入 halt。
 */
void arch_exit(int code)
{
    outb(0xF4, (uint8_t)code);
    arch_halt();
}

const char *arch_name(void)
{
    return "i386";
}

unsigned long arch_irq_save(void)
{
    unsigned long flags;
    /* pushf 把 eflags 推上 stack 再彈到暫存器;然後 cli 關中斷 */
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags));
    return flags;
}

void arch_irq_restore(unsigned long flags)
{
    if (flags & 0x200)                   /* eflags bit 9 = IF:原本是開的才打開 */
        __asm__ volatile("sti");
}

/*
 * 新執行緒的初始 stack(高位址在上),對應 switch.S 的彈出順序:
 *     0            假的返回位址:entry 不會返回,只是讓 stack 版面與一般呼叫相同
 *     entry        arch_switch 最後的 ret 會跳到這裡
 *     ebp = 0      backtrace 以 0 判斷鏈的終點
 *     ebx esi edi = 0
 * i386 ABI 要求函式進入時 (esp + 4) 為 16 的倍數,top 已對齊,
 * 所以 entry 開始執行時 esp = top - 4,符合要求。
 */
void *arch_stack_init(void *top, void (*entry)(void))
{
    uint32_t *sp = top;
    *--sp = 0;
    *--sp = (uint32_t)entry;
    *--sp = 0;                           /* ebp */
    *--sp = 0;                           /* ebx */
    *--sp = 0;                           /* esi */
    *--sp = 0;                           /* edi */
    return sp;
}

/*
 * i386 的 frame 版面(使用 -fno-omit-frame-pointer,ebp 為 frame pointer):
 *   *(ebp)     = 上一層的 ebp
 *   *(ebp + 4) = 返回位址
 * 每一步都檢查 ebp 在 stack 內、對齊、且往高位址前進。
 */
void arch_backtrace(uintptr_t pc, uintptr_t fp)
{
    int n = 0;
    uintptr_t lo, hi;

    kprintf("backtrace:\n");
    if (pc)
        kprintf("  #%d pc=%p\n", n++, (void *)pc);

    sched_stack_bounds(&lo, &hi);        /* 目前執行緒的 stack 範圍 */
    while (n < 16) {
        if (fp & 3 || fp < lo || fp + 8 > hi)
            break;
        uintptr_t prev = ((uintptr_t *)fp)[0];
        uintptr_t ra   = ((uintptr_t *)fp)[1];
        if (!ra)
            break;
        kprintf("  #%d ra=%p\n", n++, (void *)ra);
        if (prev <= fp)
            break;
        fp = prev;
    }
}

void arch_test_illegal(void)
{
    __asm__ volatile("ud2");                 /* 未定義指令,觸發 #UD */
}

void arch_test_breakpoint(void)
{
    __asm__ volatile("int3");
}

void arch_test_fault(void)
{
    /*
     * 未開啟分頁時,讀寫任何 4GB 內的位址都不會產生例外,所以改用
     * 保護錯誤:載入一個超出 GDT 範圍的區段選擇子 -> #GP,錯誤碼為該選擇子。
     */
    __asm__ volatile("mov $0x38, %%ax\n"
                     "mov %%ax, %%fs" : : : "eax");
}
