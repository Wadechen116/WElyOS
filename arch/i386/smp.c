/*
 * smp.c (i386) - 啟動其他 CPU
 *
 * x86 上這件事分三個部分:
 *
 * 1. 有哪些 CPU?
 *    沒有裝置樹,要查 BIOS 留下的 ACPI(Advanced Configuration and Power Interface)表格:
 *      RSDP(Root System Description Pointer):在 0xE0000~0xFFFFF 之間,以 "RSD PTR " 開頭
 *       -> RSDT(Root System Description Table):一串指向其他表格的位址
 *       -> MADT(Multiple APIC Description Table,簽章 "APIC"):列出每顆 CPU 的 LAPIC ID
 *    每張表都有檢查碼(所有位元組相加為 0),驗證過才使用。
 *
 * 2. 怎麼叫醒?
 *    每顆 CPU 內建一個 LAPIC(Local Advanced Programmable Interrupt Controller),
 *    暫存器對映在記憶體 0xFEE00000。寫入它的 ICR(Interrupt Command Register)
 *    可以對別顆 CPU 發出處理器間中斷(IPI)。啟動用的是兩種特殊的 IPI:
 *      INIT:把目標 CPU 重設,讓它進入「等待 SIPI」的狀態
 *      SIPI(Startup IPI):附帶一個頁號,目標 CPU 從那一頁開始以真實模式執行
 *    規範要求 INIT 之後等 10ms 再送 SIPI,並建議 SIPI 送兩次。
 *
 * 3. 醒來之後從哪裡開始?
 *    真實模式只能執行 1MB 以下的 16 位元程式,所以要先把一段跳板複製到低位址
 *    (見 ap_tramp.S),由它把 CPU 帶進保護模式、開啟分頁,再跳進 kernel。
 */
#include <stddef.h>
#include <stdint.h>
#include "arch.h"
#include "boot_info.h"
#include "cpu.h"
#include "io.h"
#include "kprintf.h"
#include "kstring.h"
#include "mmu.h"
#include "pmm.h"
#include "sched.h"
#include "smp.h"
#include "timer.h"
#include "trap.h"
#include "vm.h"

#define TRAMP_ADDR   0x8000u                 /* 跳板放置的位址;SIPI 的頁號 = 0x08 */

/* LAPIC 暫存器(相對於基底的位移) */
#define LAPIC_ID       0x020                 /* bit 31~24:這顆 CPU 的 LAPIC ID */
#define LAPIC_TPR      0x080                 /* 優先權門檻;0 = 接受所有中斷 */
#define LAPIC_EOI      0x0B0                 /* 寫入:通知目前的中斷處理完畢 */
#define LAPIC_SVR      0x0F0                 /* bit 8:啟用 LAPIC;低 8 位元:假中斷的向量 */
#define LAPIC_LVT_TMR  0x320                 /* 計時器:向量與模式 */
#define LAPIC_LVT_LINT0 0x350                /* LINT0 腳位:傳統中斷控制器(PIC)接在這裡 */
#define LAPIC_TMR_INIT 0x380                 /* 計時器的起始值(寫入即開始倒數) */
#define LAPIC_TMR_CUR  0x390                 /* 計時器目前的值 */
#define LAPIC_TMR_DIV  0x3E0                 /* 計時器的除頻 */
#define LVT_MASKED     0x00010000u
#define LVT_EXTINT     0x00000700u           /* 傳遞模式:外部中斷(由 PIC 提供向量) */
#define TMR_PERIODIC   0x00020000u           /* 倒數到 0 後自動重新開始 */
#define TMR_DIV_16     0x3u
#define LAPIC_ICR_LO   0x300                 /* 寫入這裡就送出 IPI */
#define LAPIC_ICR_HI   0x310                 /* bit 31~24:目標的 LAPIC ID */
#define ICR_INIT       0x00000500u
#define ICR_STARTUP    0x00000600u
#define ICR_ASSERT     0x00004000u
#define ICR_LEVEL      0x00008000u
#define ICR_ALL_OTHERS 0x000C0000u           /* 目標簡寫:除了自己以外的所有 CPU */
#define ICR_BUSY       0x00001000u           /* 上一個 IPI 還沒送出去 */

extern const uint8_t ap_tramp_start[], ap_tramp_end[];   /* ap_tramp.S */

/* 跳板要讀的參數(開啟分頁前以實體位址直接存取,所以必須是全域變數) */
uint32_t ap_boot_cr0, ap_boot_cr3, ap_boot_cr4, ap_boot_efer, ap_boot_stack;
static struct cpu *ap_boot_cpu;              /* 正在啟動的是哪一顆 */

static volatile uint32_t *lapic;             /* LAPIC 暫存器的位址;NULL = 沒找到 */
static uint32_t lapic_tick_count;            /* LAPIC 計時器倒數多少相當於一個 tick */
static uint64_t rdtsc64(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/*
 * 啟用「目前這顆 CPU」的 LAPIC(每顆 CPU 各有一個,對映在同一個位址)。
 * 啟用後才能接收一般的處理器間中斷與自己的計時器中斷。
 *   開機的 CPU:傳統的計時器(PIT)經由 PIC 接在它的 LINT0 腳位,要維持「外部中斷」模式
 *   其他 CPU  :LINT0 遮蔽掉,PIC 的中斷只由開機的 CPU 處理
 */
static void lapic_enable(int bsp)
{
    lapic[LAPIC_TPR / 4] = 0;
    if (bsp) {
        uint32_t l0 = lapic[LAPIC_LVT_LINT0 / 4];
        if ((l0 & LVT_MASKED) || (l0 & 0x700) != LVT_EXTINT)
            lapic[LAPIC_LVT_LINT0 / 4] = LVT_EXTINT;
    } else {
        lapic[LAPIC_LVT_LINT0 / 4] = LVT_MASKED;
    }
    lapic[LAPIC_SVR / 4] = 0x100 | VEC_SPURIOUS;
}

/*
 * 量測 LAPIC 計時器的速度:它的頻率因機器而異,必須用已知的時鐘校正。
 * 讓它從最大值開始倒數,用 TSC 等一個 tick 的時間,看它少了多少。
 * (沒有 TSC 時改用系統的 tick 計數,量兩個 tick 取平均)
 */
static void lapic_timer_calibrate(void)
{
    uint64_t per_tick = arch_tsc_per_tick();

    lapic[LAPIC_LVT_TMR / 4] = LVT_MASKED;
    lapic[LAPIC_TMR_DIV / 4] = TMR_DIV_16;
    if (per_tick) {
        lapic[LAPIC_TMR_INIT / 4] = 0xFFFFFFFFu;
        uint64_t t0 = rdtsc64();
        while (rdtsc64() - t0 < per_tick)
            arch_cpu_relax();
        lapic_tick_count = 0xFFFFFFFFu - lapic[LAPIC_TMR_CUR / 4];
    } else {
        uint64_t t = timer_ticks();
        while (timer_ticks() == t)
            arch_cpu_relax();
        lapic[LAPIC_TMR_INIT / 4] = 0xFFFFFFFFu;
        while (timer_ticks() < t + 3)
            arch_cpu_relax();
        lapic_tick_count = (0xFFFFFFFFu - lapic[LAPIC_TMR_CUR / 4]) / 2;
    }
    lapic[LAPIC_TMR_INIT / 4] = 0;           /* 停止 */
    if (lapic_tick_count == 0)
        lapic_tick_count = 1;
}
static int tramp_mapped;

/* ---- ACPI ---- */

/* 確保 [pa, pa+len) 可以讀取:不在頁表裡的頁以唯讀對映(恆等對映)。
   ACPI 表格在 BIOS 保留的區域,不屬於 kernel 管理的記憶體,平常沒有對映 */
static const uint8_t *phys(uint32_t pa, uint32_t len)
{
    for (uint32_t p = pa & ~(PAGE_SIZE - 1); p < pa + len; p += PAGE_SIZE)
        if (vm_query(p, NULL, NULL) != 0 && vm_map(p, p, VM_READ) != 0)
            return NULL;
    return (const uint8_t *)(uintptr_t)pa;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int checksum_ok(const uint8_t *p, uint32_t len)
{
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++)
        sum = (uint8_t)(sum + p[i]);
    return sum == 0;
}

/* 取得一張 ACPI 表格(含標頭):對映、檢查長度與檢查碼。失敗回傳 NULL */
static const uint8_t *acpi_table(uint32_t pa, uint32_t *len)
{
    const uint8_t *t = phys(pa, 36);         /* 標頭 36 位元組;偏移 4 是整張表的長度 */
    if (!t)
        return NULL;
    *len = rd32(t + 4);
    if (*len < 36 || *len > 0x10000 || !phys(pa, *len) || !checksum_ok(t, *len))
        return NULL;
    return t;
}

/* 找出 MADT。失敗回傳 NULL */
static const uint8_t *find_madt(uint32_t *len)
{
    const uint8_t *bios = phys(0xE0000, 0x20000);
    if (!bios)
        return NULL;

    for (uint32_t off = 0; off < 0x20000; off += 16) {
        const uint8_t *rsdp = bios + off;
        if (memcmp(rsdp, "RSD PTR ", 8) != 0 || !checksum_ok(rsdp, 20))
            continue;

        uint32_t rlen;
        const uint8_t *rsdt = acpi_table(rd32(rsdp + 16), &rlen);
        if (!rsdt || memcmp(rsdt, "RSDT", 4) != 0)
            return NULL;
        for (uint32_t i = 36; i + 4 <= rlen; i += 4) {
            const uint8_t *t = acpi_table(rd32(rsdt + i), len);
            if (t && memcmp(t, "APIC", 4) == 0)
                return t;
        }
        return NULL;
    }
    return NULL;
}

/* ---- LAPIC ---- */

static void lapic_wait(void)
{
    while (lapic[LAPIC_ICR_LO / 4] & ICR_BUSY)
        arch_cpu_relax();
}

static void lapic_ipi(uint32_t hi, uint32_t lo)
{
    lapic_wait();
    lapic[LAPIC_ICR_HI / 4] = hi;
    lapic[LAPIC_ICR_LO / 4] = lo;            /* 寫入低半部時才真的送出 */
    lapic_wait();
}

/* 很短的延遲:讀取 port 0x80(傳統上沒有裝置,讀一次約 1 微秒) */
static void short_delay(unsigned n)
{
    while (n--)
        (void)inb(0x80);
}

int arch_smp_discover(const struct boot_info *bi, uint64_t *ids, int max)
{
    uint32_t len;
    const uint8_t *madt = find_madt(&len);
    int n = 0;

    if (!madt) {
        kprintf("smp: no ACPI MADT found, staying on one cpu\n");
        return 0;
    }

    /* 跳板要放在 0x8000。那裡是 BIOS 交給作業系統使用的低位址記憶體,
       但 bootloader 可能把開機資訊放在附近,先確認沒有重疊 */
    for (unsigned i = 0; i < bi->n_reserved; i++) {
        uint64_t b = bi->reserved[i].base, e = b + bi->reserved[i].size;
        if (b < TRAMP_ADDR + PAGE_SIZE && e > TRAMP_ADDR) {
            kprintf("smp: trampoline page is in use by the bootloader, staying on one cpu\n");
            return 0;
        }
    }

    /* MADT:偏移 36 是 LAPIC 的位址,偏移 44 起是一筆筆長度不一的紀錄(類型、長度、內容) */
    uint32_t base = rd32(madt + 36);
    if (vm_map(base, base, VM_READ | VM_WRITE | VM_DEVICE) != 0)
        return 0;
    lapic = (volatile uint32_t *)(uintptr_t)base;
    uint32_t self = lapic[LAPIC_ID / 4] >> 24;
    cpus[0].arch_id = self;
    lapic_enable(1);
    lapic_timer_calibrate();

    for (uint32_t off = 44; off + 2 <= len; ) {
        uint8_t type = madt[off], rlen = madt[off + 1];
        if (rlen < 2 || off + rlen > len)
            break;                           /* 損壞的紀錄:不再往下讀 */
        /* 類型 0 = 一顆 CPU 的 LAPIC:[2] ACPI 編號、[3] LAPIC ID、[4..7] 旗標(bit 0 = 可用) */
        if (type == 0 && rlen >= 8 && (rd32(madt + off + 4) & 1) &&
            madt[off + 3] != self && n < max)
            ids[n++] = madt[off + 3];
        off += rlen;
    }
    if (n == 0)
        return 0;

    /* 複製跳板:先以可寫對映、複製完改成唯讀可執行(W^X:不留下可寫又可執行的頁) */
    size_t tlen = (size_t)(ap_tramp_end - ap_tramp_start);
    if (tlen > PAGE_SIZE ||
        vm_map(TRAMP_ADDR, TRAMP_ADDR, VM_READ | VM_WRITE) != 0)
        return 0;
    memcpy((void *)(uintptr_t)TRAMP_ADDR, ap_tramp_start, tlen);
    if (vm_map(TRAMP_ADDR, TRAMP_ADDR, VM_READ | VM_EXEC) != 0)
        return 0;
    tramp_mapped = 1;

    /* AP 要使用和這顆 CPU 完全相同的控制暫存器設定 */
    __asm__ volatile("mov %%cr0, %0" : "=r"(ap_boot_cr0));
    __asm__ volatile("mov %%cr4, %0" : "=r"(ap_boot_cr4));
    ap_boot_cr3 = (uint32_t)vm_kernel_root();    /* 不用目前的 CR3:它可能是某個行程的頁表 */
    ap_boot_efer = 0;
    if (arch_mmu_has_nx()) {
        uint32_t hi;
        __asm__ volatile("rdmsr" : "=a"(ap_boot_efer), "=d"(hi) : "c"(0xC0000080u));
    }

    /* INIT 一次送給所有其他 CPU,只需要等一次 10ms(逐顆送就要等 N 次) */
    lapic_ipi(0, ICR_ALL_OTHERS | ICR_INIT | ICR_ASSERT | ICR_LEVEL);
    lapic_ipi(0, ICR_ALL_OTHERS | ICR_INIT | ICR_LEVEL);         /* 解除 */
    /* 規範要求等 10ms 讓實體 CPU 完成重設。在模擬器 / 虛擬機裡重設是瞬間完成的,
       不必等那麼久:CPUID 功能位元 ECX bit 31 由虛擬機管理程式設定,實體 CPU 上恆為 0 */
    uint32_t eax = 1, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    thread_sleep((ecx & (1u << 31)) ? 1 : 10);
    return n;
}

int arch_smp_start(struct cpu *c)
{
    if (!lapic)
        return -1;
    ap_boot_cpu = c;
    ap_boot_stack = (uint32_t)c->kstack_top;

    uint32_t dest = (uint32_t)c->arch_id << 24;
    lapic_ipi(dest, ICR_STARTUP | (TRAMP_ADDR >> 12));
    short_delay(200);
    /* 第二次:第一次若沒被收到就靠這一次;已經啟動的 CPU 會忽略它 */
    if (!c->started)
        lapic_ipi(dest, ICR_STARTUP | (TRAMP_ADDR >> 12));
    return 0;
}

void arch_smp_done(void)
{
    /* 跳板用完了:移除對映,低位址不再留著一頁可執行的程式 */
    if (tramp_mapped) {
        vm_unmap(TRAMP_ADDR);
        tramp_mapped = 0;
    }
}

/* AP 的第一段 C 程式:由跳板呼叫,此時已在自己的 stack 上、分頁已開,
   但用的還是跳板的臨時 GDT,this_cpu() 還不能用 */
void arch_ap_start(void)
{
    trap_init_ap(ap_boot_cpu->id);           /* kernel 的 GDT、IDT 與自己的 TSS */
    vm_enable_secondary();                   /* 確認 CR4 / CR3 / CR0 與開機的 CPU 一致 */
    smp_ap_main();
}

void arch_smp_cpu_init(void)
{
    lapic_enable(0);
}

/* 啟動「目前這顆 AP」的計時器:LAPIC 內建的計時器,週期性地發出向量 48 的中斷。
   (開機的 CPU 繼續使用 PIT。系統時間由任何一顆 CPU 依 TSC 推進,見 kernel/timer.c 與 I-337) */
void arch_timer_init_cpu(void)
{
    lapic[LAPIC_TMR_DIV / 4] = TMR_DIV_16;
    lapic[LAPIC_LVT_TMR / 4] = VEC_LAPIC_TIMER | TMR_PERIODIC;
    lapic[LAPIC_TMR_INIT / 4] = lapic_tick_count;
}

/* 發出處理器間中斷:固定向量 49,目標以 LAPIC ID 指定 */
void arch_ipi_send(struct cpu *c)
{
    if (lapic)
        lapic_ipi((uint32_t)c->arch_id << 24, VEC_IPI);
}

/* 由 trap_handler 在向量 48、49 時呼叫 */
void lapic_interrupt(uint32_t vector)
{
    sched_irq_enter();
    if (vector == VEC_LAPIC_TIMER)
        timer_tick();                        /* 扣時間片;TSC 已過下一個 tick 時也推進系統時間 */
    else
        smp_ipi_handler();
    lapic[LAPIC_EOI / 4] = 0;                /* 回應之後 LAPIC 才會再送下一個 */
    sched_irq_exit();                        /* 必須在 EOI 之後:這裡可能切換到別的執行緒 */
}

void arch_smp_park(void)
{
    for (;;)
        __asm__ volatile("cli; hlt");
}
