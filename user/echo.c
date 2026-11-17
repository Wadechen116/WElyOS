/*
 * echo.c - 把參數印出來(以空白分隔,最後換行)
 *
 * 回傳值是收到的參數個數(含程式名稱),測試用它確認參數有正確傳到。
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
    sys_write(1, buf, len);
    return argc;
}
