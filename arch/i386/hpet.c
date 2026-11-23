/*
 * hpet.c (i386) - HPET 主計數器:所有 CPU 共用的備援時鐘(I-341)
 *
 * 為什麼需要:I-337 讓每顆 CPU 依共用計數器推進系統時間。i386 的首選是 TSC(讀一次約數十個週期),
 * 但 TSC 可能不存在(CPUID 沒有)、或各 CPU 不同步(clksync 驗證不合格)。以前這兩種情形都退回
 * 「只有第 0 顆推進」的單點。HPET 是晶片組上的「一個」計數器,所有 CPU 讀的是同一個硬體,
 * 本質上不會有跨 CPU 偏移,適合當備援。
 *
 * 為什麼選 HPET 不選 ACPI PM timer(取捨記在 DEVLOG):
 *   - 寬度:QEMU 的 HPET 是 64 位元(GCAP bit 13),不會繞回;PM timer 只有 24(或 32)位元,
 *     3.579545 MHz 下 24 位元約 4.7 秒就繞回,必須「每次」都做軟體延伸,而且讀者之間要協調。
 *   - 解析度:QEMU 的 HPET 週期 10 ns(100 MHz);PM timer 279 ns。tick 換算的捨去誤差也較小。
 *   - 讀取成本:兩者都是 uncached 的裝置存取(MMIO / port I/O),在模擬器上都要離開翻譯後的程式碼。
 *     i386 讀 64 位元 HPET 要三次 32 位元讀(高、低、高),比 PM timer 的一次多,但 PM timer 在部分晶片組
 *     有已知錯誤要讀三次取中間值,實際相當。每顆 CPU 每 tick 只讀一次,1000 Hz × 4 顆 × 3 次可接受。
 *   - QEMU 可用性:pc 機器預設提供 HPET(0xFED00000,ACPI 有 "HPET" 表格);可用 -machine hpet=off 拿掉,
 *     正好當負對照。
 * HPET 不在或不合格時,clksync 仍退回只由第 0 顆推進(並記錄);PM timer 留作日後第二層備援(ISSUES)。
 *
 * 32 位元的 HPET(GCAP bit 13 = 0,真實硬體可能有;QEMU 沒有):以 (ext_hi, ext_last) 延伸成 64 位元。
 * 讀者:以序號取一致的一對,值 = ext_hi:ext_last + (uint32_t)(lo − ext_last)。只要距上次更新不到 2^32 個計數
 * (100 MHz 下約 42.9 秒)就正確;經過超過 2^30 個計數時,讀者試著(trylock,不等待)更新一對。
 * 每顆 CPU 每 tick 都讀時鐘,所以實際更新間隔約 10.7 秒,遠在 42.9 秒之內。
 * 測試(clock=hpet32)把 QEMU 的 64 位元計數器當 32 位元用,並把起始值設在繞回前幾秒,讓這段程式真的被執行。
 *
 * 64 位元除法:i386 沒有 __udivdi3。頻率 = 10^15 / 週期(fs)以逐位元長除法算(只用移位、比較、減法)。
 */
#include <stdint.h>
#include "arch.h"
#include "cpu.h"              /* arch_cpu_relax */
#include "kprintf.h"
#include "vm.h"
#include "acpi.h"
#include "hpet.h"

#define HPET_GCAP      0x000      /* 低 32 位元:能力 */
#define HPET_PERIOD    0x004      /* GCAP 的高 32 位元:主計數器週期(femtoseconds) */
#define HPET_CONF      0x010
#define HPET_CNT_LO    0x0F0
#define HPET_CNT_HI    0x0F4

#define GCAP_COUNT64   (1u << 13) /* 主計數器是 64 位元 */
#define CONF_ENABLE    (1u << 0)  /* 主計數器開始計數 */
#define CONF_LEGACY    (1u << 1)  /* 傳統路由:HPET 取代 PIT 的 IRQ 0 — 我們靠 PIT,必須保持 0 */
#define PERIOD_MAX     0x05F5E100u   /* 規格上限 100 ns */
#define FS_PER_SEC     1000000000000000ULL

static volatile uint32_t *regs;
static int is32;

/* 32 位元延伸的狀態 */
static volatile uint32_t ext_seq;            /* 奇數 = 寫入中 */
static volatile uint32_t ext_hi, ext_last;
static volatile uint32_t ext_busy;           /* 更新權(trylock);持有者關中斷 */
static volatile uint32_t ext_wraps;

static inline uint32_t rd(unsigned off)
{
    return regs[off / 4];
}

static inline void wr(unsigned off, uint32_t v)
{
    regs[off / 4] = v;
}

/* 64 位元除以 32 位元:逐位元長除法,不需要 libgcc。只在開機時用一次 */
static uint64_t udiv64_32(uint64_t n, uint32_t d)
{
    uint64_t q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) {
            r -= d;
            q |= 1ULL << i;
        }
    }
    return q;
}

/* 64 位元硬體:高、低、高;兩次高位元相同,低位元就沒有在中間進位 */
static uint64_t read64(void)
{
    uint32_t hi, lo, hi2;
    do {
        hi = rd(HPET_CNT_HI);
        lo = rd(HPET_CNT_LO);
        hi2 = rd(HPET_CNT_HI);
    } while (hi != hi2);
    return ((uint64_t)hi << 32) | lo;
}

static void ext_update(void)
{
    if (__atomic_exchange_n(&ext_busy, 1, __ATOMIC_ACQUIRE))
        return;                              /* 別顆 CPU 正在更新:不等 */
    unsigned long fl = arch_irq_save();      /* 持有更新權時不能被同一顆 CPU 的讀者打斷(序號為奇數會自旋) */
    uint32_t hi = ext_hi, last = ext_last;
    uint32_t lo = rd(HPET_CNT_LO);
    uint64_t v = (((uint64_t)hi << 32) | last) + (uint32_t)(lo - last);
    __atomic_store_n(&ext_seq, ext_seq + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    ext_hi = (uint32_t)(v >> 32);
    ext_last = (uint32_t)v;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&ext_seq, ext_seq + 1, __ATOMIC_RELAXED);
    if ((uint32_t)(v >> 32) != hi)
        __atomic_store_n(&ext_wraps, ext_wraps + 1, __ATOMIC_RELAXED);
    arch_irq_restore(fl);
    __atomic_store_n(&ext_busy, 0, __ATOMIC_RELEASE);
}

static uint64_t read_ext(void)
{
    for (;;) {
        uint32_t s = __atomic_load_n(&ext_seq, __ATOMIC_ACQUIRE);
        if (s & 1) {
            arch_cpu_relax();
            continue;
        }
        uint32_t hi = ext_hi, last = ext_last;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&ext_seq, __ATOMIC_RELAXED) != s)
            continue;
        uint32_t lo = rd(HPET_CNT_LO);       /* 在取得一對「之後」讀:lo − last 不會是負的(差 2^32 以內) */
        uint32_t delta = lo - last;
        if (delta >= (1u << 30))
            ext_update();                    /* 過了四分之一圈:更新基準(失敗也無妨,下一個讀者會再試) */
        return (((uint64_t)hi << 32) | last) + delta;
    }
}

uint64_t hpet_read(void)
{
    return is32 ? read_ext() : read64();
}

int hpet_is32(void)
{
    return is32;
}

uint32_t hpet_wraps(void)
{
    return __atomic_load_n(&ext_wraps, __ATOMIC_RELAXED);
}

uint64_t hpet_init(int force32, uint64_t wrap_after_ms)
{
    uint32_t len;
    const uint8_t *t = acpi_find_table("HPET", &len);
    if (!t || len < 56) {
        kprintf("hpet: no ACPI HPET table\n");
        return 0;
    }
    /* 偏移 40 起是 Generic Address Structure:[40] 位址空間(0 = 記憶體)、[44..51] 64 位元位址 */
    uint32_t base = (uint32_t)t[44] | ((uint32_t)t[45] << 8) | ((uint32_t)t[46] << 16) | ((uint32_t)t[47] << 24);
    uint32_t base_hi = (uint32_t)t[48] | ((uint32_t)t[49] << 8) | ((uint32_t)t[50] << 16) | ((uint32_t)t[51] << 24);
    if (t[40] != 0 || base_hi != 0 || base == 0 || (base & 0x3FF)) {
        kprintf("hpet: unusable address (space %u, 0x%x%08x)\n", t[40], base_hi, base);
        return 0;
    }
    /* 裝置暫存器:不可快取(VM_DEVICE 在 i386 設 PCD|PWT,見 mmu.c)。已有對映時必須可寫而且是裝置屬性 */
    unsigned fl = 0;
    if (vm_query(base, NULL, &fl) == 0) {
        if ((fl & (VM_WRITE | VM_DEVICE)) != (VM_WRITE | VM_DEVICE)) {
            kprintf("hpet: 0x%x already mapped with flags 0x%x\n", base, fl);
            return 0;
        }
    } else if (vm_map(base, base, VM_READ | VM_WRITE | VM_DEVICE) != 0) {
        kprintf("hpet: cannot map 0x%x\n", base);
        return 0;
    }
    regs = (volatile uint32_t *)(uintptr_t)base;

    uint32_t cap = rd(HPET_GCAP), period = rd(HPET_PERIOD);
    if (period == 0 || period > PERIOD_MAX) {
        kprintf("hpet: bad period %u fs\n", period);
        regs = NULL;
        return 0;
    }
    uint64_t hz = udiv64_32(FS_PER_SEC, period);
    if (hz > 0xFFFFFFFFu) {                  /* 週期 < 232829 fs(> 4.29 GHz):tick 換算要 32 位元除法,不支援 */
        kprintf("hpet: period %u fs too short\n", period);
        regs = NULL;
        return 0;
    }
    is32 = !(cap & GCAP_COUNT64) || force32;

    uint32_t conf = rd(HPET_CONF) & ~CONF_LEGACY;   /* 保持 PIT 的 IRQ 0 路由 */
    if (force32) {
        /* 測試:停住、把計數器設在「繞回前 wrap_after_ms 毫秒」,再啟動。只有停住時可寫(規格) */
        wr(HPET_CONF, conf & ~CONF_ENABLE);
        uint32_t ahead = ((uint32_t)hz / 1000u) * (uint32_t)wrap_after_ms;   /* 32 位元除法;呼叫端保證不溢位 */
        wr(HPET_CNT_LO, 0u - ahead);
        wr(HPET_CNT_HI, 0);
    }
    wr(HPET_CONF, conf | CONF_ENABLE);
    if (is32) {
        ext_hi = 0;
        ext_last = rd(HPET_CNT_LO);
    }
    /* 確認真的在走(速率由 timer.c 以 PIT 交叉驗證) */
    uint64_t a = hpet_read();
    for (int i = 0; i < 1000 && hpet_read() == a; i++)
        arch_cpu_relax();
    if (hpet_read() == a) {
        kprintf("hpet: main counter does not advance\n");
        regs = NULL;
        return 0;
    }
    kprintf("hpet: at 0x%x, %s counter, period %u fs (%llu Hz)%s\n", base, (cap & GCAP_COUNT64) ? "64-bit" : "32-bit",
            period, (unsigned long long)hz, force32 ? ", forced 32-bit (test)" : "");
    return hz;
}
