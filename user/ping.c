/*
 * ping.c - 測試另一台機器是否連得到,以及一來一回要多久
 * 用法:ping [-c 次數] 位址        位址是四個數字,例如 ping 10.0.2.2(閘道)
 *
 * 它送出 ICMP(Internet Control Message Protocol)的 echo request,對方依規定原樣回覆。
 * 回傳 0 = 至少收到一個回答。
 */
#include "libc.h"

/* "10.0.2.2" -> 位址;格式不對回傳 0 並把 *ok 設為 0 */
static unsigned parse_ip(const char *s, int *ok)
{
    unsigned ip = 0;
    int parts = 0;

    *ok = 0;
    while (parts < 4) {
        unsigned v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9' && digits < 3) {
            v = v * 10 + (unsigned)(*s++ - '0');
            digits++;
        }
        if (!digits || v > 255)
            return 0;
        ip = (ip << 8) | v;
        parts++;
        if (parts < 4 && *s++ != '.')
            return 0;
    }
    *ok = *s == 0;
    return ip;
}

int main(int argc, char **argv)
{
    int count = 4, arg = 1, ok, got = 0;

    if (argc > arg + 1 && strcmp(argv[arg], "-c") == 0) {
        count = atoi(argv[arg + 1]);
        arg += 2;
    }
    unsigned ip = argc > arg ? parse_ip(argv[arg], &ok) : (ok = 0);
    if (!ok || count < 1 || count > 100) {
        fprintf(2, "usage: ping [-c COUNT] A.B.C.D\n");
        return 2;
    }
    for (int i = 0; i < count; i++) {
        long ms = sys_ping(ip, 1000);
        if (ms >= 0) {
            printf("reply from %s: time %ld ms\n", argv[arg], ms);
            got++;
        } else if (ms == -E_NETDOWN) {
            printf("ping: no network\n");
            return 1;
        } else {
            printf("no reply from %s (%s)\n", argv[arg],
                   ms == -E_TIMEDOUT ? "timed out" :
                   ms == -E_BUSY ? "still looking up its hardware address; try again" :
                   ms == -E_INVAL ? "this address cannot be pinged" : "error");
        }
        if (i + 1 < count)
            sys_sleep(200);
    }
    printf("%d sent, %d received\n", count, got);
    return got ? 0 : 1;
}
