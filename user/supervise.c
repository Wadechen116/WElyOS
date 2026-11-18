/*
 * supervise.c - 執行一個程式,它不正常結束時重新啟動它
 * 用法:supervise [-n 次數] 命令 [參數...]      -n:最多重新啟動幾次(預設 3)
 *
 * 長時間執行的服務遲早會遇到沒料到的狀況而當掉。與其假設它永遠不會出錯,
 * 不如安排好「出錯之後怎麼辦」:有人看著它,當掉就重新啟動,並留下紀錄。
 * (航太飛行軟體中負責這件事的元件通常稱為 health and safety。)
 *
 * 規則:
 *   - 程式以 0 結束:工作完成,supervise 也以 0 結束
 *   - 以其他結束碼結束、或被訊號或例外終止:等一下再重新啟動,每次等的時間加倍
 *     (馬上重來通常只會馬上再失敗一次;加倍等待避免佔滿 CPU 與紀錄)
 *   - 重新啟動的次數用完:放棄,以最後一次的結束碼結束。一直重來不會讓問題消失,
 *     而且會把真正的原因淹沒在大量的紀錄裡
 * 每一次重新啟動與放棄都記進黑盒子(需要系統管理的特權;沒有的話只印在畫面上)。
 */
#include "libc.h"

int main(int argc, char **argv)
{
    char path[128], note[100];
    int max = 3, arg = 1, st = 0;
    long delay = 50;

    if (argc > arg + 1 && strcmp(argv[arg], "-n") == 0) {
        max = atoi(argv[arg + 1]);
        arg += 2;
    }
    if (argc <= arg || max < 0 || max > 100) {
        fprintf(2, "usage: supervise [-n MAX-RESTARTS] COMMAND [ARGS...]\n");
        return 2;
    }
    snprintf(path, sizeof(path), "%s%s", strchr(argv[arg], '/') ? "" : "/bin/", argv[arg]);

    for (int restarts = 0; ; restarts++) {
        long pid = sys_spawn(path, argv + arg);
        if (pid < 0) {
            fprintf(2, "supervise: cannot start %s\n", argv[arg]);
            return 127;
        }
        while (sys_waitpid(pid, &st, 0) == -E_INTR)
            ;
        if (st == 0)
            return 0;
        if (restarts >= max) {
            snprintf(note, sizeof(note), "%s: exit status %d, giving up after %d restart(s)", argv[arg],
                     st, restarts);
            printf("supervise: %s\n", note);
            sys_bblog(BB_RESTART, note);
            return st;
        }
        snprintf(note, sizeof(note), "%s: exit status %d, restart %d of %d in %ld ms", argv[arg], st,
                 restarts + 1, max, delay);
        printf("supervise: %s\n", note);
        sys_bblog(BB_RESTART, note);
        sys_sleep(delay);
        if (delay < 5000)
            delay *= 2;
    }
}
