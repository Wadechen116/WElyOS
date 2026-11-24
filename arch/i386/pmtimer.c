/*
 * pmtimer.c (i386) - ACPI PM timer:HPET 之後的第二層共用備援時鐘(I-341)
 *
 * 為什麼需要:HPET 不在(-machine hpet=off、或真實硬體沒有)或不合格時,以前直接退回「只有第 0 顆推進
 * 系統時間」的單點。ACPI PM timer 和 HPET 一樣是晶片組上的「一個」計數器,所有 CPU 讀同一個硬體,
 * 本質上沒有跨 CPU 偏移;幾乎所有 ACPI 機器都有,所以當最後一層。
 *
 * 為什麼排在 HPET 之後(取捨見 hpet.c 開頭):寬度只有 24 位元(FADT 的 TMR_VAL_EXT = 1 時 32 位元),
 * 3.579545 MHz 下 24 位元約 4.69 秒就繞回,每次都要軟體延伸;解析度 279 ns,比 HPET 粗。
 *
 * 位置:FADT(簽章 "FACP")的 PM_TMR_BLK(偏移 76,4 位元組 I/O port);ACPI 2.0 以後另有 X_PM_TMR_BLK
 * (偏移 208 的 Generic Address Structure)。只支援 I/O 位址空間(QEMU 與一般 PC 都是)。
 * 開機時不需要開 ACPI 模式(SCI_EN):PM timer 在上電後就一直在數,這點以「確認在走」實際驗證。
 *
 * 延伸成 64 位元(和 hpet.c 的 32 位元延伸同一個做法,只多一個遮罩):
 *   讀者以序號取一致的一對 (ext_hi, ext_last)「之後」才讀硬體 raw,
 *   值 = ext_hi:ext_last + ((raw − ext_last) & mask)。
 *   延伸值與硬體值對 2^寬度 同餘,所以 (raw − ext_last) & mask 就是從上次更新到現在經過的計數,
 *   條件是距上次更新不到一整圈(24 位元 4.69 秒)。經過四分之一圈(1.17 秒)時讀者試著更新(trylock,不等)。
 *   每顆 CPU 每個 tick 都會讀時鐘,所以要「所有 CPU 同時超過 3.5 秒沒讀」才會少算一圈 — 只有整台機器
 *   被暫停(除錯器、宿主機停住虛擬機器)才會;後果是時間落後一圈,不會倒退(DEVLOG 記錄這個代價)。
 *   只用加、減、遮罩,沒有除法。
 *
 * 已知的晶片組錯誤(部分 PIIX4 / ICH 讀到不穩定的值,要讀三次取中間值):這裡不處理 — QEMU 是讀取時
 * 才計算值,沒有這個問題;真實硬體要用時再加(ISSUES)。
 */
#include <stdint.h>
#include "arch.h"
#include "cpu.h"              /* arch_cpu_relax */
#include "io.h"
#include "kprintf.h"
#include "acpi.h"
#include "pmtimer.h"

#define FADT_PM_TMR_BLK   76
#define FADT_PM_TMR_LEN   91
#define FADT_FLAGS        112
#define FADT_X_PM_TMR_BLK 208
#define FLAG_TMR_VAL_EXT  (1u << 8)   /* 1 = 32 位元計數器 */

static uint16_t port;
static uint32_t mask;
static int is24;

/* 延伸的狀態(見檔頭) */
static volatile uint32_t ext_seq;            /* 奇數 = 寫入中 */
static volatile uint32_t ext_hi, ext_last;
static volatile uint32_t ext_busy;           /* 更新權(trylock);持有者關中斷 */
static volatile uint32_t ext_wraps;

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void ext_update(void)
{
    if (__atomic_exchange_n(&ext_busy, 1, __ATOMIC_ACQUIRE))
        return;                              /* 別顆 CPU 正在更新:不等 */
    unsigned long fl = arch_irq_save();      /* 持有更新權時不能被同一顆 CPU 的讀者打斷(序號為奇數會自旋) */
    uint32_t hi = ext_hi, last = ext_last;
    uint32_t raw = inl(port);
    uint64_t v = (((uint64_t)hi << 32) | last) + ((raw - last) & mask);
    __atomic_store_n(&ext_seq, ext_seq + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    ext_hi = (uint32_t)(v >> 32);
    ext_last = (uint32_t)v;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&ext_seq, ext_seq + 1, __ATOMIC_RELAXED);
    /* 以前只看 (v ^ last) & ~mask,32 位元時 ~mask = 0、永遠不會計數(Codex 006) */
    if (pmtimer_crossed(v, hi, last, is24))
        __atomic_store_n(&ext_wraps, ext_wraps + 1, __ATOMIC_RELAXED);
    arch_irq_restore(fl);
    __atomic_store_n(&ext_busy, 0, __ATOMIC_RELEASE);
}

uint64_t pmtimer_read(void)
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
        uint32_t raw = inl(port);            /* 在取得一對「之後」讀:經過量不會是負的(一圈以內) */
        uint32_t delta = (raw - last) & mask;
        if (delta >= (mask >> 2) + 1)
            ext_update();                    /* 過了四分之一圈:更新基準(失敗也無妨,下一個讀者會再試) */
        return (((uint64_t)hi << 32) | last) + delta;
    }
}

int pmtimer_is24(void)
{
    return is24;
}

uint32_t pmtimer_wraps(void)
{
    return __atomic_load_n(&ext_wraps, __ATOMIC_RELAXED);
}

uint32_t pmtimer_init(void)
{
    uint32_t len;
    const uint8_t *t = acpi_find_table("FACP", &len);
    if (!t || len < FADT_PM_TMR_LEN + 1) {
        kprintf("pmtimer: no ACPI FADT\n");
        return 0;
    }
    uint32_t blk = rd32le(t + FADT_PM_TMR_BLK);
    uint32_t flags = len >= FADT_FLAGS + 4 ? rd32le(t + FADT_FLAGS) : 0;   /* 太短:當 24 位元(安全的一方) */
    int xused = 0;
    /* X_PM_TMR_BLK:寬度、偏移、存取大小都要符合「用 inl 讀整個 32 位元暫存器」才用,
       不符就忽略 X 欄位、退回舊的 PM_TMR_BLK(Codex 006;檢查見 pmtimer_gas_ok) */
    uint32_t xport;
    if (len >= FADT_X_PM_TMR_BLK + 12 && pmtimer_gas_ok(t + FADT_X_PM_TMR_BLK, &xport)) {
        blk = xport;
        xused = 1;
    }
    kprintf("pmtimer: FADT rev %u, length %u, PM_TMR_BLK 0x%x%s, PM_TMR_LEN %u, flags 0x%x\n",
            t[8], len, blk, xused ? " (X_PM_TMR_BLK)" : "", t[FADT_PM_TMR_LEN], flags);
    if (blk == 0 || blk > 0xFFFCu || t[FADT_PM_TMR_LEN] != 4) {
        kprintf("pmtimer: unusable (port 0x%x, length %u)\n", blk, t[FADT_PM_TMR_LEN]);
        return 0;
    }
    port = (uint16_t)blk;
    is24 = !(flags & FLAG_TMR_VAL_EXT);
    mask = is24 ? 0x00FFFFFFu : 0xFFFFFFFFu;
    ext_hi = 0;
    ext_last = inl(port) & mask;
    /* 確認真的在走(速率由 timer.c 以 PIT 交叉驗證) */
    uint64_t a = pmtimer_read();
    for (int i = 0; i < 1000 && pmtimer_read() == a; i++)
        arch_cpu_relax();
    if (pmtimer_read() == a) {
        kprintf("pmtimer: counter does not advance\n");
        return 0;
    }
    kprintf("pmtimer: port 0x%x, %u-bit counter\n", port, is24 ? 24u : 32u);
    return PMTIMER_HZ;
}
