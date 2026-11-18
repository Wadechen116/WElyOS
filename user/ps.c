/*
 * ps.c - 列出目前所有的行程
 * 用法:ps
 *
 * 欄位:
 *   PID    行程編號            PPID   父行程的編號(0 = 沒有)
 *   PGID   行程群組            THR    執行緒數
 *   STATE  run    有執行緒正在執行或等著執行
 *          sleep  全部在等待(等輸入、等時間到、等子行程結束…)
 *          zombie 已經結束,父行程還沒來取結束碼
 *          stop   被暫停了(Ctrl-Z 或 kill -STOP),等 fg / bg / kill -CONT
 *   TIME   累計用掉的 CPU 時間(毫秒)
 *   NAME   程式名稱
 *
 * 資料來自 sys_procinfo:一次問一個,用上一個的編號接著問下一個。
 */
#include "ulib.h"

/* 把數字靠右對齊成 width 個字元寬 */
static void line_pad_num(struct line *l, long v, int width)
{
    int digits = 1;

    for (long t = v; t >= 10; t /= 10)
        digits++;
    for (; digits < width; digits++)
        line_str(l, " ");
    line_num(l, v);
}

int main(void)
{
    struct procinfo pi;
    long after = 0;

    puts_("  PID  PPID  PGID  THR  STATE     TIME  NAME\n");
    while (sys_procinfo(after, &pi) == 0) {
        struct line l = { .len = 0 };

        after = pi.pid;
        line_pad_num(&l, pi.pid, 5);
        line_pad_num(&l, pi.ppid, 6);
        line_pad_num(&l, pi.pgid, 6);
        line_pad_num(&l, pi.nthreads, 5);
        line_str(&l, pi.state == PROC_RUNNING ? "  run   " :
                     pi.state == PROC_SLEEPING ? "  sleep " :
                     pi.state == PROC_STOPPED ? "  stop  " : "  zombie");
        line_pad_num(&l, (long)pi.cpu_ticks, 7);
        line_str(&l, "  ");
        line_str(&l, pi.name);
        line_str(&l, "\n");
        line_out(&l);
    }
    return 0;
}
