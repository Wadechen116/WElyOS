/*
 * faultlog.c - 顯示黑盒子裡的事件紀錄(最新的在最上面)
 * 用法:faultlog [筆數]      預設 20 筆;-a 全部
 *
 * 這些紀錄存在磁碟上、檔案系統以外的一小塊區域,重新開機後還在。
 * 系統當掉之後重新開機,用它查看上一次發生了什麼事。
 * 欄位:開機次數、時間(UTC)、類型、內容。
 */
#include "libc.h"

int main(int argc, char **argv)
{
    static const char *const types[] = { "?", "boot", "shutdown", "PANIC", "WATCHDOG", "fault",
                                         "DISK", "restart", "note", "healthy" };
    long max = argc > 1 ? (strcmp(argv[1], "-a") == 0 ? 1000 : atoi(argv[1])) : 20;
    struct bb_record r;
    long i = 0;

    for (; i < max && sys_bbread(i, &r) == 0; i++) {
        struct datetime dt;
        gmtime_(r.time, 0, &dt);
        r.text[r.len < sizeof(r.text) ? r.len : sizeof(r.text) - 1] = 0;
        printf("#%-3u %04d-%02d-%02d %02d:%02d:%02d  %-8s  %s\n", r.boot, dt.year, dt.mon, dt.day,
               dt.hour, dt.min, dt.sec, r.type <= BB_HEALTHY ? types[r.type] : "?", r.text);
    }
    if (i == 0)
        printf("faultlog: no records (no disk, or the disk has no log area)\n");
    return 0;
}
