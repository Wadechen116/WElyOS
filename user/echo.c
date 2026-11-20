/*
 * echo.c - 把參數印出來(以空白分隔,最後換行)
 *
 * 成功回傳 0;寫不出去回傳 1。
 * (第 50 步以前回傳參數個數給測試用,結果在 shell 裡每一個 echo 都被當成失敗;
 *  那個用途改由 /bin/args 負責,見 args.c)
 */
#include "ulib.h"

int main(int argc, char **argv)
{
    char buf[256];
    unsigned len = 0;

    /* 先組成一整行再一次寫出,輸出才不會被其他行程的輸出從中間切開 */
    for (int i = 1; i < argc; i++) {
        if (i > 1 && len < sizeof(buf) - 1)
            buf[len++] = ' ';
        for (const char *s = argv[i]; *s && len < sizeof(buf) - 1; s++)
            buf[len++] = *s;
    }
    buf[len++] = '\n';
    (void)argc;
    if (sys_write(1, buf, len) != (long)len)
        return 1;                                /* 寫不出去(例如管線另一端已關閉、磁碟滿了) */
    return 0;
}
