/*
 * cat.c - 把檔案的內容輸出到標準輸出
 * 用法:cat [檔案...]   沒有給檔案就把標準輸入原樣輸出(可以接在管線中間)。
 * 全部成功回傳 0;任何一個檔案打不開回傳 1
 */
#include "ulib.h"

/*
 * 把 fd 的內容全部複製到標準輸出。回傳 0 = 全部完成;1 = 讀取失敗;2 = 寫不出去。
 * 只寫了一部分(例如管線暫時滿了)就接著寫剩下的;寫入回傳 0 或負值才算失敗。
 * 讀取回傳負值是錯誤,不是檔案結尾 — 不可以把「讀到一半壞掉」當成「讀完了」。
 */
static int copy_out(long fd)
{
    char buf[512];
    long n;

    while ((n = sys_read(fd, buf, sizeof(buf))) > 0) {
        for (long done = 0; done < n; ) {
            long w = sys_write(1, buf + done, (unsigned long)(n - done));
            if (w <= 0)
                return 2;
            done += w;
        }
    }
    return n < 0 ? 1 : 0;
}

int main(int argc, char **argv)
{
    int rc = 0;

    if (argc < 2) {                              /* 沒有參數:讀標準輸入直到結束 */
        int err = copy_out(0);
        if (err == 1)
            puts_("cat: read error\n");
        return err ? 1 : 0;                      /* 寫不出去時連訊息也多半印不出來:只回傳失敗 */
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
        int err = copy_out(fd);
        sys_close(fd);
        if (err == 1) {
            puts_("cat: read error: ");
            puts_(argv[i]);
            puts_("\n");
        }
        if (err) {
            rc = 1;
            if (err == 2)
                break;                           /* 輸出已經壞了:後面的檔案也寫不出去 */
        }
    }
    return rc;
}
