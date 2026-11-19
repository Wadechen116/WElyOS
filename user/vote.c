/*
 * vote.c - 把一個命令執行三份,投票決定結果(三重執行,見 tmr.h)
 * 用法:vote [-t 毫秒] [-k N] 命令 [參數…]
 *   -t 毫秒  每一份最多執行多久(預設 5000);超過的會被終止,算作出錯的那一份
 *   -k N     示範用:第 N 份(1~3)一開始就被終止,模擬其中一份當掉
 * 例如:vote wc /etc/motd
 *       vote -k 2 cat /etc/motd       有一份當掉,結果仍然正確
 *
 * 三份的輸出與結束碼完全相同:照常輸出。只有兩份相同:輸出那兩份的結果,
 * 在標準錯誤說明哪一份不同,並記入黑盒子(faultlog 看得到)。三份都不同:不輸出任何結果。
 * 結束碼:命令本身的結束碼;125 = 沒有兩份相同;124 = 無法執行。
 *
 * 注意:命令必須是「重複執行會得到相同結果」的 — 讀檔、計算可以;
 * 會修改檔案或輸出時間的命令,三份本來就可能不同。
 */
#include "libc.h"
#include "tmr.h"

static int kill_which = -1;

static int run_command(void *arg)
{
    char **argv = arg;
    char path[160];

    if (strchr(argv[0], '/') || strlen(argv[0]) > sizeof(path) - 6)
        snprintf(path, sizeof(path), "%s", argv[0]);
    else
        snprintf(path, sizeof(path), "/bin/%s", argv[0]);
    long rc = sys_exec(path, argv);
    fprintf(2, "vote: cannot run %s (error %ld)\n", argv[0], -rc);
    return 127;
}

static void kill_one(const long pid[TMR_N], void *arg)
{
    (void)arg;
    if (kill_which >= 0 && sys_kill(pid[kill_which], SIGKILL) != 0)
        fprintf(2, "vote: could not stop replica %d\n", kill_which + 1);
}

int main(int argc, char **argv)
{
    unsigned timeout = 5000;
    struct tmr_result res[TMR_N];
    struct tmr_report rep;
    int i = 1;

    for (; i + 1 < argc && argv[i][0] == '-'; i += 2) {
        if (argv[i][1] == 't' && atoi(argv[i + 1]) > 0)
            timeout = (unsigned)atoi(argv[i + 1]);
        else if (argv[i][1] == 'k' && atoi(argv[i + 1]) >= 1 && atoi(argv[i + 1]) <= TMR_N)
            kill_which = atoi(argv[i + 1]) - 1;
        else
            break;
    }
    if (i >= argc || argv[i][0] == '-') {
        fprintf(2, "usage: vote [-t MILLISECONDS] [-k REPLICA] COMMAND [ARGUMENTS...]\n");
        return 124;
    }
    int code;

    if (tmr_run(run_command, &argv[i], timeout, kill_which >= 0 ? kill_one : 0, 0, res, &rep) != 0) {
        if (rep.pid[TMR_N - 1] == 0) {
            fprintf(2, "vote: could not start three replicas\n");
            code = 124;
            goto out;
        }
        fprintf(2, "vote: no two replicas agree (exit codes %d, %d, %d)%s; no result\n",
                res[0].status, res[1].status, res[2].status, rep.timed_out ? ", timed out" : "");
        code = 125;
        goto out;
    }
    const struct tmr_result *w = &res[rep.winner];
    /* 相同不代表正確:三份都超過時間、都被終止,結果也是「相同」的 — 那不是答案 */
    if (rep.timed_out && w->status == 128 + SIGKILL) {
        fprintf(2, "vote: the command did not finish within %u ms; no result\n", timeout);
        code = 125;
        goto out;
    }
    if (w->len && sys_write(1, w->out, w->len) != (long)w->len) {
        code = 124;
        goto out;
    }
    if (rep.agree == 2) {
        char note[100];
        const struct tmr_result *o = &res[rep.odd];
        snprintf(note, sizeof(note), "vote: replica %d of '%s' disagreed (exit code %d vs %d); outvoted",
                 rep.odd + 1, argv[i], o->status, w->status);
        fprintf(2, "%s%s\n", note, rep.timed_out ? " (it was stopped after the time limit)" : "");
        if (sys_bblog(BB_NOTE, note) != 0)
            fprintf(2, "vote: (not recorded in the fault log: needs the admin capability)\n");
    }
    code = w->status;
out:
    /* 三份輸出的緩衝區由 tmr_run 配置、由呼叫者釋放(沒有配置成功的是 NULL,free 不做事) */
    for (int k = 0; k < TMR_N; k++)
        free(res[k].out);
    return code;
}
