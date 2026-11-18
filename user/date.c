/*
 * date.c - 顯示現在的日期與時間
 * 用法:date            以 UTC(世界協調時間)顯示
 *       date +8         加上時差,例如台灣是 +8;也可以是負的(date -5)
 *
 * kernel 只提供「1970-01-01 00:00:00 UTC 至今的秒數」(sys_time),
 * 換算成年月日是函式庫的事(libc.h 的 gmtime_)。系統沒有時區的設定,所以時差要自己給。
 */
#include "libc.h"

int main(int argc, char **argv)
{
    static const char *const days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    struct timeval tv;
    struct datetime dt;
    int offset = argc > 1 ? atoi(argv[1]) : 0;

    if (offset < -12 || offset > 14 || sys_time(&tv) != 0) {
        fprintf(2, "usage: date [+HOURS]   (offset from UTC, -12 to +14)\n");
        return 1;
    }
    gmtime_(tv.sec, offset, &dt);
    printf("%04d-%02d-%02d %02d:%02d:%02d %s UTC", dt.year, dt.mon, dt.day, dt.hour, dt.min, dt.sec,
           days[dt.wday]);
    if (offset)
        printf("%s%d", offset > 0 ? "+" : "", offset);
    printf("\n");
    return 0;
}
