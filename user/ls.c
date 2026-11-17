/*
 * ls.c - 列出目錄的內容(目錄名稱後面加 "/")
 * 用法:ls [目錄]   沒有給就列出根目錄。成功回傳 0
 */
#include "ulib.h"

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/";
    struct line l = { .len = 0 };
    struct dirent d;
    long fd = sys_open(path, O_RDONLY);

    if (fd < 0) {
        puts_("ls: cannot open ");
        puts_(path);
        puts_("\n");
        return 1;
    }
    while (sys_readdir(fd, &d) == 0) {
        line_str(&l, d.name);
        line_str(&l, d.type == I_DIR ? "/  " : "  ");
    }
    line_str(&l, "\n");
    line_out(&l);
    sys_close(fd);
    return 0;
}
