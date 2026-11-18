/*
 * ls.c - 列出目錄的內容
 * 用法:ls [-l] [目錄]     沒有給目錄就是目前的工作目錄
 *   預設      名稱排成一列,目錄後面加 "/"
 *   -l        每個項目一列:類型與權限(例如 -rw-r--r--)、擁有者、大小、最後修改的時間(UTC)、名稱
 *             類型:d 目錄、- 檔案、c 裝置;權限三組依序是擁有者、群組、其他人
 */
#include "passwd.h"

int main(int argc, char **argv)
{
    int longfmt = argc > 1 && strcmp(argv[1], "-l") == 0;
    const char *path = argc > 1 + longfmt ? argv[1 + longfmt] : ".";
    struct dirent d;
    long fd = sys_open(path, O_RDONLY);

    if (fd < 0) {
        printf("ls: cannot open %s\n", path);
        return 1;
    }
    while (sys_readdir(fd, &d) == 0) {
        if (!longfmt) {
            printf("%s%s", d.name, d.type == I_DIR ? "/  " : "  ");
            continue;
        }
        char full[320];
        struct stat st;
        struct datetime dt;

        snprintf(full, sizeof(full), "%s/%s", path, d.name);
        if (sys_stat(full, &st) != 0)
            continue;                            /* 列到一半被刪掉了 */
        char perm[11], owner[32];
        perm[0] = st.type == I_DIR ? 'd' : st.type == I_FILE ? '-' : 'c';
        for (int b = 0; b < 9; b++)
            perm[1 + b] = (st.mode >> (8 - b)) & 1 ? "rwx"[b % 3] : '-';
        perm[10] = 0;
        printf("%s %-6s %8lu  ", perm, passwd_name((long)st.uid, owner, sizeof(owner)),
               (unsigned long)st.size);
        if (st.mtime) {
            gmtime_(st.mtime, 0, &dt);
            printf("%04d-%02d-%02d %02d:%02d", dt.year, dt.mon, dt.day, dt.hour, dt.min);
        } else {
            printf("%16s", "-");                 /* 沒有記錄時間(例如開機檔案包裡的檔案) */
        }
        printf("  %s%s\n", d.name, st.type == I_DIR ? "/" : "");
    }
    if (!longfmt)
        printf("\n");
    sys_close(fd);
    return 0;
}
