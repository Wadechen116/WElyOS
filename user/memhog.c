/*
 * memhog.c - 把記憶體用到被拒絕為止(I-240 的壓力測試,第 52 步 C3.1)
 * 用法:memhog file   在 / (記憶體檔案系統)寫檔案,直到寫不進去
 *       memhog fork   不斷 fork(子行程停在管線上等待),直到 fork 失敗
 * 用到被拒絕之後保持 500ms(kernel 的測試在這段期間量空閒頁數),再全部歸還。
 * 被拒絕時是「明確的錯誤」才回傳 0;一直沒被拒絕(不應該發生)回傳 2,其他錯誤回傳 3。
 */
#include "ulib.h"

#define MAX_FILES 64
#define MAX_KIDS  4096
#define CHUNK     65536

static char buf[CHUNK];
static long kids[MAX_KIDS];

static int hog_files(void)
{
    char name[16] = "/hog00";
    int nfiles = 0, refused = 0, bad = 0;
    unsigned long kb = 0;

    for (; nfiles < MAX_FILES && !refused; nfiles++) {
        name[4] = (char)('0' + nfiles / 10);
        name[5] = (char)('0' + nfiles % 10);
        long fd = sys_open(name, O_CREAT | O_WRONLY | O_TRUNC);
        if (fd < 0) {
            refused = 1;                         /* 連建立檔案都不行:同樣是被拒絕 */
            break;
        }
        for (int i = 0; i < 15 * 16; i++) {     /* 每個檔案最多 15MB(單檔上限 16MB) */
            long n = sys_write(fd, buf, CHUNK);
            if (n > 0)
                kb += (unsigned long)n / 1024;
            if (n != CHUNK) {
                if (n < 0 && n != -E_NOMEM && n != -E_NOSPC)
                    bad = 1;                     /* 非預期的錯誤:關檔之後以 3 結束 */
                refused = 1;
                break;
            }
        }
        if ((sys_close(fd) != 0 && !refused) || bad)
            return 3;
    }
    struct line l = { .len = 0 };
    line_str(&l, "memhog: wrote ");
    line_num(&l, (long)kb);
    line_str(&l, " KB in ");
    line_num(&l, nfiles);
    line_str(&l, " files before being refused\n");
    line_out(&l);
    sys_sleep(500);
    for (int i = 0; i < nfiles; i++) {
        name[4] = (char)('0' + i / 10);
        name[5] = (char)('0' + i % 10);
        if (sys_unlink(name) != 0)
            bad = 1;
    }
    return bad ? 3 : (refused ? 0 : 2);
}

static int hog_fork(void)
{
    int fds[2];
    if (sys_pipe(fds) != 0)
        return 3;
    int n = 0, refused = 0;
    long err = 0;
    while (n < MAX_KIDS) {
        long pid = sys_fork();
        if (pid == 0) {
            char c;
            if (sys_close(fds[1]) != 0)
                sys_exit(4);
            /* 父行程關閉寫入端時回傳 0:正常結束;其他結果都算錯 */
            sys_exit(sys_read(fds[0], &c, 1) == 0 ? 0 : 5);
        }
        if (pid < 0) {
            err = pid;
            refused = 1;
            break;
        }
        kids[n++] = pid;
    }
    struct line l = { .len = 0 };
    line_str(&l, "memhog: ");
    line_num(&l, n);
    line_str(&l, " children before fork was refused (error ");
    line_num(&l, -err);
    line_str(&l, ")\n");
    line_out(&l);
    sys_sleep(500);
    int bad = sys_close(fds[1]) != 0;
    if (sys_close(fds[0]) != 0)
        bad = 1;
    for (int i = 0; i < n; i++) {
        int st = -1;
        if (sys_waitpid(kids[i], &st, 0) != kids[i] || st != 0)
            bad = 1;
    }
    if (bad)
        return 3;
    return refused && err == -E_NOMEM ? 0 : (refused ? 3 : 2);
}

int main(int argc, char **argv)
{
    if (argc == 2 && streq(argv[1], "file"))
        return hog_files();
    if (argc == 2 && streq(argv[1], "fork"))
        return hog_fork();
    puts_("usage: memhog file|fork\n");
    return 1;
}
