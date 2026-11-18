/*
 * su.c - 以另一個使用者的身分開一個 shell
 * 用法:su 使用者 [命令 參數...]   使用者可以是名稱或數字;沒有給命令就開 /bin/sh
 *       在那個 shell 裡輸入 exit 回到原本的 shell(與原本的身分)
 *
 * 這個系統裡,身分只能「往下降」:有特權的行程可以變成任何使用者,
 * 而變成一般使用者之後就再也拿不回特權(見 kernel 的 SYS_setuid)。
 * 所以 su 不需要密碼 — 沒有特權的人執行它只會得到「permission denied」。
 * 「回到 root」不是把身分升回去,而是結束這個降級後的 shell,回到一直等在那裡的父行程。
 */
#include "passwd.h"

int main(int argc, char **argv)
{
    static char *sh_argv[] = { "sh", 0 };
    struct passwd_entry e;

    if (argc < 2) {
        fprintf(2, "usage: su USER [COMMAND ARGS...]\n");
        return 1;
    }
    if (!passwd_find(argv[1], 0, &e)) {
        if (argv[1][0] < '0' || argv[1][0] > '9') {
            fprintf(2, "su: unknown user: %s\n", argv[1]);
            return 1;
        }
        e.uid = e.gid = atoi(argv[1]);
    }
    if (sys_setuid(e.uid, e.gid) != 0) {
        fprintf(2, "su: permission denied\n");
        return 1;
    }
    if (argc > 2) {
        char path[128];
        snprintf(path, sizeof(path), "%s%s", strchr(argv[2], '/') ? "" : "/bin/", argv[2]);
        sys_exec(path, argv + 2);
        fprintf(2, "su: cannot run %s\n", argv[2]);
        return 127;
    }
    sys_exec("/bin/sh", sh_argv);
    fprintf(2, "su: cannot start /bin/sh\n");
    return 127;
}
