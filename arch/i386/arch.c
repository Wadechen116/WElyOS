/*
 * arch.c (i386) - 實作 include/arch.h:序列埠 + VGA 雙輸出
 */
#include "arch.h"
#include "io.h"
#include "kprintf.h"
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

/* boot.S 定義的 stack 範圍,用來驗證 frame pointer 是否合理 */
extern char stack_bottom[], stack_top[];

/*
 * i386 的 frame 版面(使用 -fno-omit-frame-pointer,ebp 為 frame pointer):
 *   *(ebp)     = 上一層的 ebp
 *   *(ebp + 4) = 返回位址
 * 每一步都檢查 ebp 在 stack 內、對齊、且往高位址前進。
 */
void arch_backtrace(uintptr_t pc, uintptr_t fp)
{
    int n = 0;

    kprintf("backtrace:\n");
    if (pc)
        kprintf("  #%d pc=%p\n", n++, (void *)pc);

    while (n < 16) {
        if (fp & 3 || fp < (uintptr_t)stack_bottom || fp + 8 > (uintptr_t)stack_top)
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
