/*
 * rtc.c - PC 的即時時鐘(CMOS RTC)
 *
 * PC 主板上有一個由電池供電的時鐘晶片,和一小塊設定用的記憶體放在一起(俗稱 CMOS)。
 * 存取方式:把暫存器編號寫到 port 0x70,再從 port 0x71 讀出內容。
 *   0x00 秒  0x02 分  0x04 時  0x07 日  0x08 月  0x09 年(末兩位)  0x32 世紀
 *   0x0A 狀態 A:bit 7 = 正在更新(這時讀到的值可能不一致)
 *   0x0B 狀態 B:bit 2 = 數值是二進位(否則是 BCD);bit 1 = 24 小時制
 * BCD(Binary-Coded Decimal):每 4 個位元表示一位十進位數字,0x59 代表 59。
 */
#include <stdint.h>
#include "arch.h"
#include "io.h"

static uint8_t cmos(uint8_t reg)
{
    outb(0x70, reg);
    return inb(0x71);
}

struct rtc_time { unsigned sec, min, hour, day, mon, year; };

static void rtc_sample(struct rtc_time *t)
{
    for (int i = 0; i < 100000 && (cmos(0x0A) & 0x80); i++)
        ;                                    /* 等它更新完(有上限,不會卡住) */
    t->sec = cmos(0x00);
    t->min = cmos(0x02);
    t->hour = cmos(0x04);
    t->day = cmos(0x07);
    t->mon = cmos(0x08);
    t->year = cmos(0x09) | ((unsigned)cmos(0x32) << 8);
}

static unsigned bcd(unsigned v) { return (v >> 4) * 10 + (v & 15); }

/* 西元年月日換算成「1970-01-01 之後的第幾天」(公曆的規則;算法出自 Howard Hinnant 的 days_from_civil) */
static uint32_t days_from_civil(unsigned y, unsigned m, unsigned d)
{
    y -= m <= 2;
    unsigned era = y / 400, yoe = y - era * 400;
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

uint64_t arch_rtc_read(void)
{
    struct rtc_time a, b;
    uint8_t status_b = cmos(0x0B);

    /* 連續讀到兩次相同才採用:避免剛好跨過秒的進位,讀到一半新一半舊 */
    rtc_sample(&a);
    for (int i = 0; i < 10; i++) {
        rtc_sample(&b);
        if (a.sec == b.sec && a.min == b.min && a.hour == b.hour && a.day == b.day &&
            a.mon == b.mon && a.year == b.year)
            break;
        a = b;
    }
    unsigned pm = a.hour & 0x80;             /* 12 小時制時,最高位元表示下午 */
    a.hour &= 0x7F;
    if (!(status_b & 0x04)) {                /* BCD */
        a.sec = bcd(a.sec);
        a.min = bcd(a.min);
        a.hour = bcd(a.hour);
        a.day = bcd(a.day);
        a.mon = bcd(a.mon);
        a.year = bcd(a.year & 0xFF) + bcd(a.year >> 8) * 100;
    } else {
        a.year = (a.year & 0xFF) + (a.year >> 8) * 100;
    }
    if (!(status_b & 0x02))                  /* 12 小時制 */
        a.hour = a.hour % 12 + (pm ? 12 : 0);
    if (a.year < 1970)
        a.year += a.year < 100 ? 2000 : 0;   /* 沒有世紀暫存器的機器:當作 20xx */
    /* 硬體的內容不可信任:數值不合理就當作沒有時鐘 */
    if (a.year < 1970 || a.year > 2200 || a.mon < 1 || a.mon > 12 || a.day < 1 || a.day > 31 ||
        a.hour > 23 || a.min > 59 || a.sec > 60)
        return 0;
    return (uint64_t)days_from_civil(a.year, a.mon, a.day) * 86400u +
           a.hour * 3600u + a.min * 60u + a.sec;
}
