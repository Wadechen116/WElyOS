/*
 * timer.c (i386) - 8259 PIC 中斷控制器與 8253/8254 PIT 計時器
 *
 * PC 的硬體中斷(IRQ)經由兩顆串接的 8259 PIC 送到 CPU:
 *   主 PIC(port 0x20/0x21):IRQ 0~7      從 PIC(port 0xA0/0xA1):IRQ 8~15
 *   從 PIC 接在主 PIC 的 IRQ 2 上。
 *
 * 為什麼要「重新對映」:開機時 BIOS 把 IRQ 0~7 對應到向量 8~15,
 * 但保護模式下向量 0~31 保留給 CPU 例外(例如 8 是 double fault)。
 * 不改的話,計時器中斷(IRQ 0)會被當成 double fault。
 * 所以把 IRQ 0~15 改對應到向量 32~47。
 *
 * PIT 是可程式化計時器,輸入時脈固定 1193182 Hz。channel 0 接在 IRQ 0,
 * 設定除數 N 後以 1193182 / N 的頻率產生中斷(週期性,不用每次重設)。
 *
 * 計時的準確性:PIT 只會「發出中斷」,不會告訴我們現在是第幾次。如果中斷
 * 被延遲到下一次也到了(中斷關閉太久,或模擬器的主機排程延遲),兩次會合併
 * 成一次,kernel 少算一個 tick,系統時間就變慢,而且無從得知。
 * 所以另外用 TSC(time stamp counter,CPU 內建、隨時間遞增的 64 位元計數器)
 * 當作獨立的時間來源:PIT 中斷只負責「叫醒」,實際該記幾個 tick 由 TSC
 * 經過了多少時間來決定。CPU 不支援 TSC 時退回每次中斷記一個 tick。
 *
 * 共用時鐘的來源(I-341):arch_clock_count() 是 kernel/timer.c 推進系統時間用的共用計數器。
 * 首選 TSC;另外在開機時探測 HPET 主計數器(hpet.c)與 ACPI PM timer(pmtimer.c)當兩層備援。
 * 開機時選第一個可用的;不合格(clksync 驗證失敗)時由 kernel/timer.c 呼叫 arch_clock_fallback()
 * 依序切到下一層:TSC → HPET → PM timer。
 * 切換只會 TSC → HPET 一次,只在 timer.c 的序號寫入區間內做(讀者不會把兩種單位混用)。
 */
#include <stdint.h>
#include "arch.h"
#include "console.h"
#include "serial.h"
#include "io.h"
#include "kprintf.h"
#include "timer.h"
#include "sched.h"
#include "trap.h"
#include "hpet.h"
#include "pmtimer.h"

#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1
#define PIC_EOI    0x20        /* End Of Interrupt 命令 */

#define PIT_CH0    0x40
#define PIT_CMD    0x43
#define PIT_HZ     1193182u

#define IRQ_BASE   32          /* IRQ 0 對應的向量編號 */
#define IRQ_COM1   4           /* 序列埠 COM1 */

#define PIT_CH2    0x42        /* channel 2:原本接喇叭,輸出狀態可由 port 0x61 讀回 */
#define PORT_SPKR  0x61        /* bit 0 = channel 2 的閘門,bit 1 = 喇叭開關,bit 5 = channel 2 輸出 */
#define MAX_CATCHUP 100        /* 一次中斷最多補記幾個 tick */

static uint64_t tsc_interval;  /* 一個 tick 相當於多少 TSC 計數;0 = 不使用 TSC */
static uint64_t tsc_deadline;  /* 下一個 tick 應該發生的 TSC 值 */
static uint64_t tsc_hz;        /* TSC 每秒的計數(校正得到);0 = 沒有 TSC */

enum { SRC_UNSET = 0, SRC_TSC, SRC_HPET, SRC_PMTMR, SRC_NONE };
static volatile int clk_src;   /* 目前的共用時鐘;SRC_UNSET = arch_timer_init 之前(開機最早期) */
static uint64_t hpet_hz;       /* HPET 每秒計數;0 = 沒有或不合格 */
static uint64_t hpet_iv;       /* 一個 tick 相當於多少 HPET 計數 */
static int test_hpet32;        /* 測試(clock=hpet32):把 HPET 當 32 位元用,開機後約 2 秒繞回 */
static uint32_t pm_hz;         /* PM timer 每秒計數;0 = 沒有或不合格 */
static uint32_t pm_iv;         /* 一個 tick 相當於多少 PM timer 計數(無條件進位,見 pmtimer_probe) */
static int test_nopm;          /* 測試(clock=nopm):不探測 PM timer */
#define HPET32_WRAP_MS 2000u

static uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* CPUID 功能位元:EDX bit 4 = 支援 TSC */
static int cpu_has_tsc(void)
{
    uint32_t eax = 1, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    (void)ebx; (void)ecx;
    return (edx >> 4) & 1;
}

/*
 * 量測 TSC 的頻率:TSC 每秒跳幾次因 CPU 而異,必須用一個已知頻率的時鐘校正。
 * 讓 PIT 的 channel 2 倒數 10ms(單次模式),輪詢它的輸出直到變成 1,
 * 這段期間 TSC 增加的量就是「10ms 的 TSC 計數」。
 * 不用 channel 0 是因為它接著 IRQ 0,而此時中斷還沒開。
 */
static uint32_t per_10ms(uint64_t (*rd)(void))
{
    uint16_t count = PIT_HZ / 100;                       /* 10ms */

    outb(PORT_SPKR, (uint8_t)((inb(PORT_SPKR) & ~0x02) | 0x01));   /* 開閘門、關喇叭 */
    outb(PIT_CMD, 0xB0);                 /* channel 2、先低後高、模式 0(倒數到 0 後輸出變 1) */
    outb(PIT_CH2, (uint8_t)(count & 0xFF));
    outb(PIT_CH2, (uint8_t)(count >> 8));

    uint64_t t0 = rd();
    while (!(inb(PORT_SPKR) & 0x20))
        ;
    return (uint32_t)(rd() - t0);        /* 10ms 內的計數,32 位元足夠(TSC < 400 GHz、HPET < 4.29 GHz) */
}

uint64_t arch_tsc_per_tick(void)
{
    return tsc_interval;
}

/* 開機最早期(kmain 開頭)計時器還沒校正,也要能讀:是否有 TSC 第一次用到時才問 CPUID */
static uint64_t early_count(void)
{
    static int have = -1;
    if (have < 0)
        have = cpu_has_tsc();
    return have ? rdtsc() : 0;
}

/* 熱路徑:每顆 CPU 每 tick 一次。一次讀取 clk_src 加一個分支;TSC 路徑與以前相同 */
uint64_t arch_clock_count(void)
{
    switch (__atomic_load_n(&clk_src, __ATOMIC_ACQUIRE)) {
    case SRC_TSC:   return rdtsc();
    case SRC_HPET:  return hpet_read();
    case SRC_PMTMR: return pmtimer_read();
    case SRC_NONE:  return 0;
    default:        return early_count();
    }
}

/* CPUID 在所有 x86 處理器上都是序列化指令(Intel SDM:RDTSC 本身不序列化,之前的指令可能還沒做完、
   之後的讀取也可能提前)。前後各一次:讀數不會早於之前的記憶體讀取(例如看到對方的 ack),
   之後的寫入(例如發出請求)也不會早於讀數。LFENCE 要 SSE2,沒有 TSC 的老型號不一定有,所以不用 */
static void cpuid_serialize(void)
{
    uint32_t a = 0, b, c = 0, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d) : : "memory");
}

uint64_t arch_clock_count_ordered(void)
{
    int src = __atomic_load_n(&clk_src, __ATOMIC_ACQUIRE);
    if (src == SRC_HPET || src == SRC_PMTMR) {
        /* HPET 是 uncached 的記憶體讀取:x86 的讀取之間不會互相重排(和 RDTSC 不同,它本身就是讀取),
           只要擋住編譯器。PM timer 的 IN 指令:x86 的 I/O 指令依程式順序執行、不會被推測或合併,
           SDM 稱其為「近乎序列化」(確切措辭未查證);要更保守可改用 cpuid_serialize,只影響 clksync 取樣 */
        __asm__ volatile("" ::: "memory");
        uint64_t t = src == SRC_HPET ? hpet_read() : pmtimer_read();
        __asm__ volatile("" ::: "memory");
        return t;
    }
    if (!arch_clock_count())                     /* 順便確定有沒有 TSC */
        return 0;
    cpuid_serialize();
    uint64_t t = rdtsc();
    cpuid_serialize();
    return t;
}

uint64_t arch_tick_interval(void)
{
    switch (__atomic_load_n(&clk_src, __ATOMIC_ACQUIRE)) {
    case SRC_TSC:   return tsc_interval;
    case SRC_HPET:  return hpet_iv;
    case SRC_PMTMR: return pm_iv;
    default:        return 0;
    }
}

/* TSC 的校正是以 PIT 量 10ms 得到的:精度受 PIT 的 1 個計數(約 0.84 微秒)與輪詢延遲限制,約千分之一。
   HPET 的頻率來自它自己回報的週期(規格精確),PIT 只用來交叉驗證 */
uint64_t arch_clock_hz(void)
{
    switch (__atomic_load_n(&clk_src, __ATOMIC_ACQUIRE)) {
    case SRC_TSC:   return tsc_hz;
    case SRC_HPET:  return hpet_hz;
    case SRC_PMTMR: return pm_hz;
    case SRC_NONE:  return 0;
    default:        return 0;                    /* 還沒校正 */
    }
}

/*
 * 切到下一層備援時鐘(I-341):TSC → HPET → PM timer,跳過不存在的。只能由 kernel/timer.c 在序號寫入區間、
 * 持有 time_lk 時呼叫。回傳 1 = 已切換(之後 arch_clock_* 都是新來源的值);0 = 沒有下一層(維持原樣)。
 */
int arch_clock_fallback(void)
{
    int src = __atomic_load_n(&clk_src, __ATOMIC_ACQUIRE), next;
    if (src == SRC_TSC && hpet_hz)
        next = SRC_HPET;
    else if ((src == SRC_TSC || src == SRC_HPET) && pm_hz)
        next = SRC_PMTMR;
    else
        return 0;
    __atomic_store_n(&clk_src, next, __ATOMIC_RELEASE);
    return 1;
}

const char *arch_clock_name(void)
{
    switch (__atomic_load_n(&clk_src, __ATOMIC_ACQUIRE)) {
    case SRC_TSC:   return "tsc";
    case SRC_HPET:  return hpet_is32() ? "hpet32" : "hpet";
    case SRC_PMTMR: return pmtimer_is24() ? "pmtimer24" : "pmtimer";
    case SRC_NONE:  return "none";
    default:        return "early";
    }
}

void arch_clock_test_mode(int mode)
{
    test_hpet32 = (mode & ARCH_CLOCK_TEST_HPET32) != 0;
    test_nopm = (mode & ARCH_CLOCK_TEST_NOPM) != 0;
}

/*
 * 探測 PM timer,並以 PIT 量 10ms 交叉驗證(期望 35795 個計數,誤差 2% 以內)。
 * 每 tick 的計數無條件進位:3579545 / 1000 = 3579.545,捨去成 3579 會讓系統時間快 152 ppm,
 * 「至少睡 n 毫秒」就會提早(違反 I-346 的語意);進位成 3580 是慢 127 ppm,只會晚。
 */
static void pmtimer_probe(unsigned hz)
{
    if (test_nopm) {
        kprintf("pmtimer: disabled (test)\n");
        return;
    }
    uint32_t h = pmtimer_init();
    if (h == 0)
        return;
    uint32_t got = per_10ms(pmtimer_read);
    uint32_t want = h / 100u, slack = want / 50u;
    if (got + slack < want || got > want + slack) {
        kprintf("pmtimer: rate check failed: %u counts in 10 ms, expected %u; not used\n", got, want);
        return;
    }
    pm_iv = (h + hz - 1) / hz;
    pm_hz = h;
}

/* 探測 HPET,並以 PIT 量 10ms 交叉驗證它回報的頻率(誤差 2% 以內;PIT 輪詢的誤差約千分之一) */
static void hpet_probe(unsigned hz)
{
    uint64_t h = hpet_init(test_hpet32, HPET32_WRAP_MS);
    if (h == 0)
        return;
    uint32_t got = per_10ms(hpet_read);
    uint32_t want = (uint32_t)h / 100u;          /* h < 2^32(hpet_init 保證) */
    uint32_t slack = want / 50u;
    if (got + slack < want || got > want + slack) {
        kprintf("hpet: rate check failed: %u counts in 10 ms, expected %u; not used\n", got, want);
        return;
    }
    hpet_iv = ((uint32_t)h + hz - 1) / hz;     /* 無條件進位(見 pmtimer_probe) */
    hpet_hz = h;
}

static void pic_remap(void)
{
    /* ICW1:開始初始化序列(之後要連續寫入 ICW2~ICW4) */
    outb(PIC1_CMD, 0x11);
    outb(PIC2_CMD, 0x11);
    /* ICW2:向量基底 */
    outb(PIC1_DATA, IRQ_BASE);        /* IRQ 0~7  -> 向量 32~39 */
    outb(PIC2_DATA, IRQ_BASE + 8);    /* IRQ 8~15 -> 向量 40~47 */
    /* ICW3:串接方式 */
    outb(PIC1_DATA, 0x04);            /* 主 PIC:IRQ 2 接著從 PIC(位元遮罩) */
    outb(PIC2_DATA, 0x02);            /* 從 PIC:自己接在主 PIC 的 IRQ 2(編號) */
    /* ICW4:8086 模式 */
    outb(PIC1_DATA, 0x01);
    outb(PIC2_DATA, 0x01);
    /* 中斷遮罩(1 = 關閉):只開 IRQ 0(計時器),其餘全部關閉 */
    outb(PIC1_DATA, 0xFE);
    outb(PIC2_DATA, 0xFF);
}

void arch_timer_init(unsigned hz)
{
    uint32_t divisor = PIT_HZ / hz;   /* 16 位元除數,hz 至少要 19 */

    pic_remap();
    /* 0x36:channel 0、先低後高位元組、模式 3(方波,週期性)、二進位計數 */
    outb(PIT_CMD, 0x36);
    outb(PIT_CH0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH0, (uint8_t)(divisor >> 8));

    if (cpu_has_tsc()) {
        uint32_t d = per_10ms(rdtsc);
        tsc_hz = (uint64_t)d * 100;
        /* 換算成一個 tick 的計數:d * 100 / hz,無條件進位(tick 只可以比真實時間慢,否則「至少睡 n 毫秒」
           會提早,見 timer.h)。拆成兩步避免 32 位元溢位,也避開 64 位元除法(i386 上需要 libgcc 的 __udivdi3) */
        tsc_interval = (uint64_t)(d / hz) * 100 + ((d % hz) * 100 + hz - 1) / hz;
        tsc_deadline = rdtsc() + tsc_interval;
    }
    hpet_probe(hz);                              /* 有 TSC 時也探測:備援要在需要之前就準備好 */
    pmtimer_probe(hz);
    int src = tsc_interval ? SRC_TSC : hpet_hz ? SRC_HPET : pm_hz ? SRC_PMTMR : SRC_NONE;
    __atomic_store_n(&clk_src, src, __ATOMIC_RELEASE);
    /* 備援鏈:目前來源之後還有哪些(和 arch_clock_fallback 的順序相同) */
    const char *fb = "none";
    if (src == SRC_TSC && hpet_hz && pm_hz)
        fb = "hpet,pmtimer";
    else if (src == SRC_TSC && hpet_hz)
        fb = "hpet";
    else if ((src == SRC_TSC || src == SRC_HPET) && pm_hz)
        fb = "pmtimer";
    kprintf("clock: shared counter %s (%llu Hz), fallback %s\n", arch_clock_name(),
            (unsigned long long)arch_clock_hz(), fb);
}

/*
 * 讓序列埠 COM1 在收到字元時發出中斷:COM1 接在主 PIC 的 IRQ 4。
 * 多核心時這條中斷只會送到開機的那顆 CPU(PIC 的輸出只接到它),
 * 和 PIT 的計時器中斷相同。
 */
void arch_console_irq_init(void)
{
    serial_irq_enable();
    outb(PIC1_DATA, (uint8_t)(inb(PIC1_DATA) & ~(1u << IRQ_COM1)));   /* 遮罩位元清 0 = 開啟 */
}

/* 由 trap_handler 在向量 32~47 時呼叫 */
void irq_handler(uint32_t irq)
{
    sched_irq_enter();                       /* 中斷中喚醒別人時,切換要留到返回前 */

    if (irq == 0) {
        if (tsc_interval) {
            /* 依 TSC 實際經過的時間決定要記幾個 tick(通常是 1 個;
               中斷被合併時會是多個,把漏掉的補回來) */
            uint64_t now = rdtsc();
            int n = 0;
            while (tsc_deadline <= now && n < MAX_CATCHUP) {
                if (n == 0)
                    timer_tick();
                else
                    timer_tick_stolen();         /* 補記的:不算成誰的 CPU 時間 */
                tsc_deadline += tsc_interval;
                n++;
            }
            int resynced = tsc_deadline <= now;
            if (resynced)                /* 落後太多:放棄補記,重新對時 */
                tsc_deadline = now + tsc_interval;
            timer_note_catchup(n, resynced);   /* 晚到的紀錄(見 timer.h) */
        } else {
            timer_tick();
        }
    } else if (irq == IRQ_COM1) {
        console_rx();                        /* 序列埠收到字元(見 kernel/console.c) */
    } else
        kprintf("[irq] unexpected IRQ %u\n", irq);

    /* 通知 PIC 處理完畢,它才會再送下一個中斷;來自從 PIC 的要兩邊都通知 */
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);

    /* 必須在 EOI 之後:這裡可能切換到別的執行緒,很久才回來。
       若還沒 EOI 就切走,PIC 會一直不送下一個計時器中斷 */
    sched_irq_exit();
}
