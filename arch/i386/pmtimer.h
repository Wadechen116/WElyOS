#ifndef WELYOS_I386_PMTIMER_H
#define WELYOS_I386_PMTIMER_H
#include <stdint.h>

/*
 * ACPI PM timer(ACPI Power Management Timer):i386 上第二層的共用備援時鐘(I-341)。
 * 晶片組上的一個 24 或 32 位元計數器,固定 3.579545 MHz,所有 CPU 讀同一個硬體。
 */
#define PMTIMER_HZ 3579545u

/* 從 FADT 找出計數器的 I/O port 並確認在走。成功回傳 PMTIMER_HZ;沒有或不合格回傳 0。
   QEMU 的 PM timer 是 24 位元(FADT flags 0x80a5,TMR_VAL_EXT = 0),開機約 4.7 秒就繞回,
   所以延伸程式不需要測試模式就會被執行;32 位元的路徑 QEMU 測不到(ISSUES) */
uint32_t pmtimer_init(void);
/* 讀計數器(以軟體延伸成 64 位元)。pmtimer_init 成功後才可呼叫 */
uint64_t pmtimer_read(void);
/*
 * 純函式(不碰硬體,tests/host/time/pmtimer_logic_test.c 在主機上測):
 * pmtimer_gas_ok:X_PM_TMR_BLK 的 GAS(Generic Address Structure,12 位元組)是否可用 —
 *   [0] 位址空間 1 = System I/O、[1] 位元寬度 24~32、[2] 位元偏移 0、[3] 存取大小 0(未指定)或 3(dword)、
 *   [4..11] 位址:高 32 位元 0、低 32 位元非 0 且 < 0x10000。可用時把 port 寫進 *port。
 * pmtimer_crossed:延伸值由 (hi:last) 更新成 v 時是否跨過一圈(延伸值除以 2^寬度 的商變了)。
 */
static inline int pmtimer_gas_ok(const uint8_t *g, uint32_t *port)
{
    uint32_t lo = (uint32_t)g[4] | ((uint32_t)g[5] << 8) | ((uint32_t)g[6] << 16) | ((uint32_t)g[7] << 24);
    uint32_t hi = (uint32_t)g[8] | ((uint32_t)g[9] << 8) | ((uint32_t)g[10] << 16) | ((uint32_t)g[11] << 24);
    if (g[0] != 1 || g[1] < 24 || g[1] > 32 || g[2] != 0 || (g[3] != 0 && g[3] != 3))
        return 0;
    if (hi != 0 || lo == 0 || lo >= 0x10000u)
        return 0;
    *port = lo;
    return 1;
}

static inline int pmtimer_crossed(uint64_t v, uint32_t hi, uint32_t last, int is24)
{
    if ((uint32_t)(v >> 32) != hi)
        return 1;                            /* 低 32 位元進位:24 與 32 位元都算跨過 */
    return is24 && (((uint32_t)v ^ last) & 0xFF000000u) != 0;
}

/* 診斷:是否 24 位元、已延伸幾次繞回 */
int      pmtimer_is24(void);
uint32_t pmtimer_wraps(void);

#endif
