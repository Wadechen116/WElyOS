/*
 * pmtimer_logic_test.c - 在主機上測 arch/i386/pmtimer.h 的純函式(I-341 / I-348;Codex 006 指出的兩點)
 *
 * QEMU 的 PM timer 是 24 位元、FADT 是 rev 1(沒有 X_PM_TMR_BLK),所以 32 位元的繞回計數與 GAS 檢查
 * 在 QEMU 裡跑不到,在這裡直接測:
 *   1. pmtimer_crossed:模擬一個真實的 24 / 32 位元計數器,以 pmtimer.c 相同的延伸公式
 *      v = hi:last + ((raw − last) & mask) 每次前進一段,計數「跨過」的次數必須等於真實值除以 2^寬度 的商。
 *      舊寫法((v ^ last) & ~mask)在 32 位元時永遠是 0 — 內建負對照確認舊寫法在這裡會失敗。
 *   2. pmtimer_gas_ok:合格的 GAS 接受;每個欄位不合格各一例都拒絕。
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../../arch/i386/pmtimer.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static int old_crossed(uint64_t v, uint32_t last, uint32_t mask)
{
    return (((uint32_t)v ^ last) & ~mask) != 0;
}

/* 模擬:真實計數器從 start 開始,每步前進 step(< 四分之一圈,和 pmtimer.c 的更新條件一致) */
static void sim(int width, uint64_t start, uint64_t step, int steps, int use_old)
{
    uint32_t mask = width == 24 ? 0x00FFFFFFu : 0xFFFFFFFFu;
    uint64_t real = start;
    uint32_t hi = 0, last = (uint32_t)real & mask;
    unsigned wraps = 0;
    uint64_t base_q = real >> width;
    for (int i = 0; i < steps; i++) {
        real += step;
        uint32_t raw = (uint32_t)real & mask;
        uint64_t v = (((uint64_t)hi << 32) | last) + ((raw - last) & mask);
        int c = use_old ? old_crossed(v, last, mask) : pmtimer_crossed(v, hi, last, width == 24);
        wraps += (unsigned)c;
        hi = (uint32_t)(v >> 32);
        last = (uint32_t)v;
        /* 延伸值與真實值只差開機時的起點(延伸從 raw 開始) */
        if (v != real - (start - ((uint32_t)start & mask)))
            CHECK(0, "width %d: extended value %llu != expected", width, (unsigned long long)v);
    }
    unsigned want = (unsigned)((real >> width) - base_q);
    if (use_old)
        CHECK(width == 24 ? wraps == want : wraps != want,
              "negative control (old formula) width %d: wraps %u want %u", width, wraps, want);
    else
        CHECK(wraps == want, "width %d start %llu step %llu: wraps %u, want %u", width,
              (unsigned long long)start, (unsigned long long)step, wraps, want);
}

static void gas(uint8_t space, uint8_t width, uint8_t off, uint8_t acc, uint64_t addr, int want, uint32_t want_port)
{
    uint8_t g[12];
    memset(g, 0, sizeof g);
    g[0] = space; g[1] = width; g[2] = off; g[3] = acc;
    for (int i = 0; i < 8; i++)
        g[4 + i] = (uint8_t)(addr >> (8 * i));
    uint32_t port = 0xDEAD;
    int got = pmtimer_gas_ok(g, &port);
    CHECK(got == want, "gas space %u width %u off %u acc %u addr 0x%llx: got %d want %d",
          space, width, off, acc, (unsigned long long)addr, got, want);
    if (want)
        CHECK(port == want_port, "gas port 0x%x want 0x%x", port, want_port);
}

int main(void)
{
    /* 24 位元:每步 1/8 圈,跨過很多次;起點在繞回前後各一 */
    sim(24, 0x00FFFF00u, 1u << 21, 200, 0);
    sim(24, 5, (1u << 22) - 1, 100, 0);
    /* 32 位元:每步約 1/5 圈,跨過 2^32 多次 — Codex 006 的情形 */
    sim(32, 0xFFFFF000u, 0x33333333u, 40, 0);
    sim(32, 1, (1u << 30) - 1, 30, 0);
    /* 負對照:舊公式在 32 位元時數不到(24 位元時與新公式相同) */
    sim(32, 0xFFFFF000u, 0x33333333u, 40, 1);
    sim(24, 0x00FFFF00u, 1u << 21, 200, 1);

    gas(1, 32, 0, 3, 0x608, 1, 0x608);       /* 合格:32 位元、dword */
    gas(1, 24, 0, 0, 0x408, 1, 0x408);       /* 合格:24 位元、存取大小未指定 */
    gas(0, 32, 0, 3, 0x608, 0, 0);           /* 記憶體空間(MMIO):不支援 */
    gas(1, 16, 0, 3, 0x608, 0, 0);           /* 寬度太小 */
    gas(1, 64, 0, 3, 0x608, 0, 0);           /* 寬度太大 */
    gas(1, 32, 8, 3, 0x608, 0, 0);           /* 有位元偏移 */
    gas(1, 32, 0, 2, 0x608, 0, 0);           /* word 存取 */
    gas(1, 32, 0, 4, 0x608, 0, 0);           /* qword 存取 */
    gas(1, 32, 0, 3, 0, 0, 0);               /* 位址 0 */
    gas(1, 32, 0, 3, 0x10000, 0, 0);         /* 超出 I/O 空間 */
    gas(1, 32, 0, 3, 0x100000608ull, 0, 0);  /* 高 32 位元非 0 */

    printf("pmtimer logic: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
