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
    close_quiet(in);                             /* 唯讀開啟的來源檔 */
    /* 權限跟著複製(複製出來的程式才一樣可以執行);擁有者是執行 cp 的人。
       在同步之前做:權限的修改才會和內容一起寫到磁碟上 */
    struct stat st;
    int perm_failed = sys_stat(argv[1], &st) != 0 || sys_chmod(argv[2], (long)st.mode) != 0;
    /* 確定資料已經到了磁碟上才算複製成功。fsync 與 close 都要檢查;
       close 不論成不成功,描述符都已經關閉(所以即使 fsync 失敗也照樣 close,不重試) */
    long synced = sys_fsync(out);
    long closed = sys_close(out);
    if (synced == -E_INVAL)
        synced = 0;                              /* 目的是裝置(例如 /dev/null):沒有寫回這件事 */
    if (synced != 0 || closed != 0)
        return fail("write failed while saving ", argv[2]);
    if (perm_failed) {
        /* 內容已經複製好了,但權限沒有跟上:複製出來的程式可能不能執行。要讓使用者知道 */
        fail("copied, but could not copy the permissions to ", argv[2]);
        return 2;
    }
    return 0;
}
