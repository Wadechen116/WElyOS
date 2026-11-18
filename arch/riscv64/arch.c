/*
 * arch.c (RISC-V 64, QEMU virt) - 實作 include/arch.h
 *
 * RISC-V 沒有 x86 那種獨立的 I/O port,週邊都映射在記憶體位址上,
 * 以一般的 load/store 存取(volatile 避免被編譯器優化掉)。
 */
#include <stdint.h>
#include "arch.h"
#include "console.h"
#include "trap.h"
#include "cpu.h"
#include "kprintf.h"
#include "sched.h"

/* virt 機器的 NS16550A 序列埠,基底 0x10000000 */
#define UART0_BASE  0x10000000UL
#define UART_THR    0    /* 傳送資料 */
#define UART_LSR    5    /* 線路狀態:bit 5 = 傳送緩衝區空 */

/* virt 機器的 SiFive test finisher:寫入特定值即可讓 QEMU 結束 */
#define FINISHER    0x100000UL
#define FINISHER_PASS 0x5555u    /* 成功,QEMU 以 0 結束 */
#define FINISHER_FAIL 0x3333u    /* 失敗,高 16 位元為結束碼 */

static volatile uint8_t *const uart = (volatile uint8_t *)UART0_BASE;

/* QEMU 的 UART 開機即可輸出,不需設定鮑率(實機需要,日後再補)。
   這裡只啟用 FIFO(暫存器 2 寫入 0x07:啟用並清空收發緩衝):
   沒有 FIFO 時硬體只能暫存 1 個字元,kernel 每 1ms 才來取一次,
   連續進來的字元會被後面的擠掉;有了 FIFO 可以暫存 16 個 */
void arch_init(void)
{
    uart[2] = 0x07;
}

void arch_putc(char c)
{
    while (!(uart[UART_LSR] & 0x20))     /* 等待傳送緩衝區空 */
        ;
    uart[UART_THR] = (uint8_t)c;
}

/* 線路狀態暫存器 bit 0(DR,data ready):為 1 表示接收到的資料還沒被讀走 */
/* 序列埠沒有需要在輸出結束時補做的事 */
void arch_console_flush(void)
{
}

int arch_console_getc(void)
{
    if (!(uart[UART_LSR] & 0x01))
        return -1;
    return uart[UART_THR];                   /* 讀取時同一個位址是接收暫存器 */
}

/*
 * ---- 序列埠的中斷:PLIC ----
 *
 * RISC-V 的 CPU 本身只有三條中斷線(計時器、軟體、外部)。所有週邊裝置的中斷
 * 都先接到一個中斷控制器 PLIC(Platform-Level Interrupt Controller,平台層級中斷控制器),
 * 由它彙整後拉起 CPU 的「外部中斷」那一條:
 *
 *   序列埠 --(來源 10)--> PLIC --(S-mode 外部中斷,scause 中斷碼 9)--> hart
 *
 * PLIC 把「某顆 hart 的某個權限模式」稱為一個 context:QEMU virt 上
 * hart N 的 M-mode 是 context 2N、S-mode 是 context 2N+1。每個 context 各有:
 *   啟用位元    哪些來源的中斷要送給它
 *   門檻        優先權高於門檻的才會送達(0 = 全部接受)
 *   認領 / 完成  讀取 = 取得是哪個來源(同時標記為處理中);
 *                處理完把同一個編號寫回去,PLIC 才會再送下一次
 * 我們只讓「開機的那顆 hart」接收序列埠的中斷:輸入的量很小,不需要分散到多顆。
 */
#define PLIC_BASE         0x0c000000UL
#define PLIC_PRIORITY(n)  (PLIC_BASE + 4UL * (n))                 /* 來源 n 的優先權(0 = 關閉) */
#define PLIC_ENABLE(ctx)  (PLIC_BASE + 0x2000UL + 0x80UL * (ctx)) /* 啟用位元,每個來源 1 位元 */
#define PLIC_THRESHOLD(ctx) (PLIC_BASE + 0x200000UL + 0x1000UL * (ctx))
#define PLIC_CLAIM(ctx)   (PLIC_THRESHOLD(ctx) + 4)
#define IRQ_UART0         10                                      /* virt 機器上序列埠的來源編號 */
#define SIE_SEIE          (1UL << 9)                              /* sie:允許 S-mode 外部中斷 */

static unsigned long plic_ctx;               /* 開機那顆 hart 的 S-mode context */

static inline uint32_t plic_r(uintptr_t a)             { return *(volatile uint32_t *)a; }
static inline void     plic_w(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

void arch_console_irq_init(void)
{
    plic_ctx = 2 * (unsigned long)this_cpu()->arch_id + 1;
    plic_w(PLIC_PRIORITY(IRQ_UART0), 1);
    plic_w(PLIC_ENABLE(plic_ctx), 1u << IRQ_UART0);
    plic_w(PLIC_THRESHOLD(plic_ctx), 0);
    uart[1] = 0x01;                          /* UART 的中斷致能暫存器:bit 0 = 收到資料 */
    __asm__ volatile("csrs sie, %0" : : "r"(SIE_SEIE));
}

/* 由 trap_handler 在收到 S-mode 外部中斷時呼叫 */
void external_interrupt(void)
{
    uint32_t irq;

    sched_irq_enter();
    /* 一直認領到沒有為止(讀到 0):處理期間若又有新的中斷,不必再進出一次 */
    while ((irq = plic_r(PLIC_CLAIM(plic_ctx))) != 0) {
        if (irq == IRQ_UART0)
            console_rx();
        else
            kprintf("[irq] unexpected PLIC interrupt %u\n", irq);
        plic_w(PLIC_CLAIM(plic_ctx), irq);   /* 完成 */
    }
    sched_irq_exit();                        /* 讀取者被叫醒的話,在這裡切換 */
}

void arch_halt(void)
{
    arch_irq_disable();
    for (;;)
        __asm__ volatile("wfi");         /* 中斷已關閉,不會再被處理 */
}

/* sstatus 的 bit 1(SIE):S-mode 全域中斷開關 */
void arch_irq_enable(void)
{
    __asm__ volatile("csrs sstatus, %0" : : "r"(2UL));
}

void arch_irq_disable(void)
{
    __asm__ volatile("csrc sstatus, %0" : : "r"(2UL));
}

void arch_idle(void)
{
    __asm__ volatile("wfi");             /* 休眠到有中斷待處理 */
}

void arch_exit(int code)
{
    volatile uint32_t *f = (volatile uint32_t *)FINISHER;
    *f = code ? (((uint32_t)code << 16) | FINISHER_FAIL) : FINISHER_PASS;
    arch_halt();                         /* 實機上無此裝置,落入閒置 */
}

const char *arch_name(void)
{
    return "riscv64";
}

unsigned long arch_irq_save(void)
{
    unsigned long old;
    /* csrrc:讀出 sstatus 舊值,同時清除指定位元(SIE),一條指令完成 */
    __asm__ volatile("csrrc %0, sstatus, %1" : "=r"(old) : "r"(2UL));
    return old;
}

void arch_irq_restore(unsigned long flags)
{
    if (flags & 2UL)                     /* 原本 SIE 是開的才打開 */
        __asm__ volatile("csrs sstatus, %0" : : "r"(2UL));
}

/*
 * 新執行緒的初始 stack,對應 switch.S 的版面(13 個 slot,補到 112 位元組):
 *     slot 0 = ra = arch_thread_start:arch_switch 最後的 ret 會跳到這裡
 *     slot 1 = s0 = 0                :frame pointer 歸零,backtrace 以此判斷終點
 *     slot 2 = s1 = entry            :arch_thread_start 把 ra 清 0 後跳到 s1
 */
void arch_thread_start(void);            /* switch.S */

void *arch_stack_init(void *top, void (*entry)(void))
{
    uint64_t *sp = (uint64_t *)top - 14;     /* 112 位元組 */
    for (int i = 0; i < 14; i++)
        sp[i] = 0;
    sp[0] = (uint64_t)arch_thread_start;
    sp[2] = (uint64_t)entry;
    return sp;
}

/*
 * RISC-V 的 frame 版面(使用 -fno-omit-frame-pointer):s0 = fp 指向 frame 頂端,
 *   *(fp - 8)  = 返回位址 ra
 *   *(fp - 16) = 上一層的 fp
 * 沿著這條鏈往回走。每一步都檢查 fp 在 stack 內、對齊、且往高位址前進,
 * 避免遇到損壞的鏈而再次出錯。
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
        if (fp & 7 || fp < lo + 16 || fp > hi)
            break;
        uintptr_t ra   = ((uintptr_t *)fp)[-1];
        uintptr_t prev = ((uintptr_t *)fp)[-2];
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
    __asm__ volatile(".4byte 0xffffffff");   /* 全 1 不是合法指令編碼 */
}

void arch_test_breakpoint(void)
{
    __asm__ volatile("ebreak");
}

void arch_test_fault(void)
{
    volatile int *p = (volatile int *)0;     /* virt 機器的位址 0 沒有裝置 */
    (void)*p;
}

/* RISC-V 從使用者模式進入 kernel 時,trap.S 直接讀 struct cpu 的 kstack_top
   (排程器每次切換時更新),不需要另外通知硬體 */
void arch_set_kstack(uintptr_t top)
{
    (void)top;
}

void arch_user_return(uintptr_t entry, uintptr_t usp, uintptr_t arg)
    __attribute__((noreturn));          /* switch.S */

void arch_enter_user(uintptr_t entry, uintptr_t usp, uintptr_t arg)
{
    arch_irq_disable();                  /* 設定 sscratch 到 sret 之間不可被中斷 */
    arch_user_return(entry, usp, arg);
}

/* 磁碟:QEMU virt 機器的 virtio-mmio 裝置在 0x10001000 起的 8 個位置,每個 4KB */
#include "blk.h"
struct blkdev *arch_blk_probe(void)
{
    return virtio_blk_probe(0x10001000UL, 0x1000, 8);
}

/* 即時時鐘:QEMU virt 機器的 Goldfish RTC 在 0x101000。讀取位移 0 得到「1970 年至今的奈秒數」
   的低 32 位元(同時把高 32 位元鎖存起來),位移 4 讀出高 32 位元 */
uint64_t arch_rtc_read(void)
{
    volatile uint32_t *rtc = (volatile uint32_t *)0x101000UL;
    uint64_t lo = rtc[0], hi = rtc[1];

    return ((hi << 32) | lo) / 1000000000ull;
}
