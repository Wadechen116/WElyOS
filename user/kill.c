/*
 * kill.c - 送訊號給行程
 * 用法:kill pid              送 SIGTERM(15):終止
 *       kill -9 pid           以編號指定:2(INT)、9(KILL)、15(TERM)終止;
 *                             19(STOP)、20(TSTP)暫停;18(CONT)繼續
 *       kill -STOP pid        也可以用名稱:INT、KILL、TERM、STOP、TSTP、CONT
 *       kill -9 -5            pid 寫成負數:送給整個行程群組(這裡是群組 5)
 * 被終止的行程,結束碼是 128 + 訊號編號。
 */
#include "ulib.h"

int main(int argc, char **argv)
{
    long sig = SIGTERM;
    int i = 1, rc = 0;

    if (argc > 1 && argv[1][0] == '-') {
        static const struct { const char *name; long sig; } names[] = {
            { "INT", SIGINT }, { "KILL", SIGKILL }, { "TERM", SIGTERM },
            { "STOP", SIGSTOP }, { "TSTP", SIGTSTP }, { "CONT", SIGCONT },
        };
        sig = uatoi(argv[1] + 1);
        for (unsigned k = 0; k < sizeof(names) / sizeof(names[0]); k++)
            if (streq(argv[1] + 1, names[k].name))
                sig = names[k].sig;
        i = 2;
    }
    if (i >= argc) {
        puts_("usage: kill [-SIGNAL] PID...\n");
        return 1;
    }
    for (; i < argc; i++) {
        /* 負數 = 行程群組 */
        long pid = argv[i][0] == '-' ? -uatoi(argv[i] + 1) : uatoi(argv[i]);
        if (sys_kill(pid, sig) != 0) {
            puts_("kill: cannot signal ");
            puts_(argv[i]);
            puts_("\n");
            rc = 1;
        }
    }
    return rc;
}
