/*
 * sleep.c - 等一段時間再結束
 * 用法:sleep 秒數        例如 sleep 3
 *       sleep 毫秒ms      例如 sleep 200ms
 * 用來測試「程式執行到一半被終止」:執行 sleep 5 之後按 Ctrl-C。
 */
#include "ulib.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        puts_("usage: sleep SECONDS | sleep MILLISECONDSms\n");
        return 1;
    }
    const char *s = argv[1];
    long n = uatoi(s);
    while (*s >= '0' && *s <= '9')
        s++;
    long ms = (s[0] == 'm' && s[1] == 's') ? n : n * 1000;

    /* 一次系統呼叫最多睡 60 秒,較長的時間分段睡 */
    while (ms > 0) {
        long step = ms > 60000 ? 60000 : ms;
        sys_sleep(step);
        ms -= step;
    }
    return 0;
}
