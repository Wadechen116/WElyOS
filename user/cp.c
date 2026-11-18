/*
 * cp.c - 複製檔案
 * 用法:cp 來源 目的          目的檔已經存在的話,內容會被換掉
 * 例如把程式放到磁碟上:cp /bin/echo /disk/echo
 */
#include "ulib.h"

static int fail(const char *what, const char *name)
{
    puts_("cp: ");
    puts_(what);
    puts_(name);
    puts_("\n");
    return 1;
}

int main(int argc, char **argv)
{
    char buf[512];
    long n;

    if (argc != 3) {
        puts_("usage: cp SOURCE DEST\n");
        return 1;
    }
    long in = sys_open(argv[1], O_RDONLY);
    if (in < 0)
        return fail("cannot open ", argv[1]);
    long out = sys_open(argv[2], O_CREAT | O_WRONLY | O_TRUNC);
    if (out < 0)
        return fail("cannot create ", argv[2]);
    while ((n = sys_read(in, buf, sizeof(buf))) > 0) {
        /* 寫入的量比要求的少,表示空間用完了(或出錯) */
        if (sys_write(out, buf, (unsigned long)n) != n)
            return fail("write failed (no space left?): ", argv[2]);
    }
    if (n < 0)
        return fail("read error: ", argv[1]);
    sys_close(out);                              /* 關閉時 kernel 把內容寫回磁碟 */
    sys_close(in);
    return 0;
}
