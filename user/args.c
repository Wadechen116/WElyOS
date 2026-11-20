/*
 * args.c - 測試用:把參數印出來(和 echo 一樣),結束碼是收到的參數個數(含程式名稱)
 *
 * 自我測試用它確認參數有正確傳到新的程式(spawn、exec、重新導向、管線)。
 * 以前這個角色由 echo 兼任,結果 echo 的結束碼不是 0 — 在 shell 裡每一個 echo 都被
 * 當成失敗(第 50 步發現:腳本中 "echo start" 的狀態是 2)。現在 echo 照慣例成功回傳 0,
 * 需要「參數個數」的測試改用這個程式。
 */
#include "ulib.h"

int main(int argc, char **argv)
{
    char buf[256];
    unsigned len = 0;

    for (int i = 1; i < argc; i++) {
        if (i > 1 && len < sizeof(buf) - 1)
            buf[len++] = ' ';
        for (const char *s = argv[i]; *s && len < sizeof(buf) - 1; s++)
            buf[len++] = *s;
    }
    buf[len++] = '\n';
    if (sys_write(1, buf, len) != (long)len)
        return 255;                              /* 寫不出去:不是參數個數,明確的失敗 */
    return argc;
}
