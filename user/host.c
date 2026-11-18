/*
 * host.c - 由名稱查出 IP 位址(DNS 查詢)
 * 用法:host 名稱 [名稱伺服器的位址]      例如 host example.com
 *       名稱伺服器預設是 10.0.2.3(模擬器提供的;它把查詢轉給主機的名稱伺服器)
 *
 * DNS(Domain Name System)的一次查詢就是一來一回兩個 UDP 封包,送到伺服器的 53 埠。
 * 訊息的組合與解析在 dns.h(純函式,不做任何系統呼叫);這個程式負責收送:
 *   - 每次查詢用一個不容易猜到的編號,回答的編號必須相同
 *   - 只接受「來自我們問的那台伺服器、53 埠」的回答
 *   - UDP 不保證送達:等不到就重問,最多 3 次,每次等 1 秒
 * 回傳 0 = 查到了;1 = 這個名稱沒有位址;2 = 用法錯誤、沒有網路或伺服器沒有回應。
 */
#include "libc.h"
#include "dns.h"

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
    static uint8_t query[300], reply[1472];
    unsigned server = NET_IP(10, 0, 2, 3), from_ip = 0, from_port = 0;
    struct timeval tv;
    size_t qlen = 0;
    uint32_t addr = 0;
    int ok = 1;

    if (argc > 2)
        server = parse_ip(argv[2], &ok);
    if (argc < 2 || !ok) {
        fprintf(2, "usage: host NAME [SERVER-ADDRESS]\n");
        return 2;
    }
    /* 查詢的編號:取自時鐘的微秒與行程編號,每次不同(回答必須帶著同一個編號才算數) */
    sys_time(&tv);
    uint16_t id = (uint16_t)(tv.usec ^ (tv.usec >> 16) ^ ((unsigned)sys_getpid() << 7) ^ (unsigned)tv.sec);

    if (dns_build_query(query, sizeof(query), id, argv[1], &qlen) != 0) {
        fprintf(2, "host: not a valid name: %s\n", argv[1]);
        return 2;
    }
    long sock = sys_udp_open(0);
    if (sock < 0) {
        fprintf(2, "host: %s\n", sock == -E_NETDOWN ? "no network" : "cannot open a socket");
        return 2;
    }
    for (int tries = 0; tries < 3; tries++) {
        if (sys_udp_send(sock, server, 53, query, (unsigned)qlen) < 0)
            break;
        for (;;) {
            long n = sys_udp_recv(sock, reply, sizeof(reply), &from_ip, &from_port, 1000);
            if (n < 0)
                break;                           /* 逾時:重問 */
            if (from_ip != server || from_port != 53)
                continue;                        /* 不是我們問的那台伺服器:不理 */
            int rc = dns_parse_a(reply, (size_t)n, id, argv[1], &addr);
            if (rc == 0) {
                printf("%s has address %u.%u.%u.%u\n", argv[1], addr >> 24, (addr >> 16) & 255,
                       (addr >> 8) & 255, addr & 255);
                return 0;
            }
            if (rc == -2) {
                printf("%s has no address\n", argv[1]);
                return 1;
            }
            /* -1:格式不對、編號或問題不相符、伺服器回報錯誤 — 不採用,繼續等真正的回答 */
        }
    }
    fprintf(2, "host: no usable answer from the name server\n");
    return 2;
}
