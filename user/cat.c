/*
 * cat.c - 把檔案的內容輸出到標準輸出
 * 用法:cat [檔案...]   沒有給檔案就把標準輸入原樣輸出(可以接在管線中間)。
 * 全部成功回傳 0;任何一個檔案打不開回傳 1
 */
#include "ulib.h"

int main(int argc, char **argv)
{
    char buf[256];
    int rc = 0;
    long n;

    if (argc < 2) {                              /* 沒有參數:讀標準輸入直到結束 */
        while ((n = sys_read(0, buf, sizeof(buf))) > 0)
            sys_write(1, buf, (unsigned long)n);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        long fd = sys_open(argv[i], O_RDONLY);
        if (fd < 0) {
            puts_("cat: cannot open ");
            puts_(argv[i]);
            puts_("\n");
            rc = 1;
            continue;
        }
        while ((n = sys_read(fd, buf, sizeof(buf))) > 0)
            sys_write(1, buf, (unsigned long)n);
        sys_close(fd);
    }
    return rc;
}
