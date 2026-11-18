/*
 * tftp.c - 從 TFTP 伺服器下載一個檔案
 * 用法:tftp 遠端的檔名 [存成的檔名]      沒有給第二個就存成同名(只取最後一段)
 * 例如:tftp etc/motd /disk/motd
 *
 * TFTP(Trivial File Transfer Protocol)是建立在 UDP 上、最簡單的檔案傳輸協定。
 * 模擬器在閘道(10.0.2.2,埠 69)上內建了一個,分享主機的一個目錄(見 scripts/qemu.sh),
 * 所以這是把檔案從主機放進 WElyOS 最方便的方法。
 *
 * 流程:
 *   我們 -> 伺服器的 69 埠     讀取要求(檔名、"octet" 表示原樣傳送)
 *   伺服器 -> 我們             資料 第 1 塊(最多 512 位元組),從伺服器「另外挑的一個埠」送出
 *   我們 -> 那個埠             確認 第 1 塊
 *   …                          不滿 512 位元組的那一塊是最後一塊
 * UDP 不保證送達,所以每一步等不到就重送,最多 4 次。
 * 只接受「來自同一個位址、同一個埠」的資料:別的來源冒充的封包一律不理。
 */
#include "libc.h"

#define SERVER NET_IP(10, 0, 2, 2)
#define OP_RRQ  1
#define OP_DATA 3
#define OP_ACK  4
#define OP_ERR  5

int main(int argc, char **argv)
{
    static unsigned char pkt[4 + 512 + 8], req[160];
    unsigned ip, port, server_port = 0, block = 1, total = 0;
    const char *local;
    unsigned reqlen = 2;

    if (argc < 2 || strlen(argv[1]) > 120) {
        fprintf(2, "usage: tftp REMOTE-FILE [LOCAL-FILE]\n");
        return 2;
    }
    local = argc > 2 ? argv[2] : (strrchr(argv[1], '/') ? strrchr(argv[1], '/') + 1 : argv[1]);

    long sock = sys_udp_open(0);
    if (sock < 0) {
        fprintf(2, "tftp: %s\n", sock == -E_NETDOWN ? "no network" : "cannot open a socket");
        return 1;
    }
    long out = sys_open(local, O_CREAT | O_WRONLY | O_TRUNC);
    if (out < 0) {
        fprintf(2, "tftp: cannot create %s\n", local);
        return 1;
    }

    /* 讀取要求:操作碼 1、檔名、0、"octet"、0 */
    req[0] = 0;
    req[1] = OP_RRQ;
    /* 檔名的長度在開頭已經檢查過(最多 120);用明確長度的複製,不用 strcpy */
    unsigned namelen = (unsigned)strlen(argv[1]) + 1;
    memcpy(req + reqlen, argv[1], namelen);
    reqlen += namelen;
    memcpy(req + reqlen, "octet", 6);
    reqlen += 6;

    for (;;) {
        long n = -1;
        /* 送出(第一次是要求,之後是對上一塊的確認),然後等下一塊;等不到就重送 */
        for (int tries = 0; tries < 4 && n < 0; tries++) {
            if (block == 1) {
                sys_udp_send(sock, SERVER, 69, req, reqlen);
            } else {
                unsigned char ack[4] = { 0, OP_ACK, (unsigned char)((block - 1) >> 8),
                                         (unsigned char)(block - 1) };
                sys_udp_send(sock, SERVER, server_port, ack, 4);
            }
            for (;;) {
                n = sys_udp_recv(sock, pkt, sizeof(pkt), &ip, &port, 500);
                if (n < 0)
                    break;                       /* 逾時:重送 */
                /* 來源必須是伺服器;第一塊決定之後要用的埠,其後必須一致 */
                if (ip != SERVER || (server_port && port != server_port) || n < 4)
                    continue;
                if (pkt[1] == OP_ERR) {
                    pkt[n < (long)sizeof(pkt) ? n : (long)sizeof(pkt) - 1] = 0;
                    fprintf(2, "tftp: server says: %s\n", n > 4 ? (char *)pkt + 4 : "error");
                    sys_close(out);
                    sys_unlink(local);
                    return 1;
                }
                if (pkt[1] == OP_DATA && (((unsigned)pkt[2] << 8) | pkt[3]) == (block & 0xFFFF))
                    break;                       /* 就是正在等的那一塊 */
                /* 其他(例如重複的舊資料塊):繼續等 */
            }
        }
        if (n < 0) {
            fprintf(2, "tftp: no answer from the server\n");
            sys_close(out);
            sys_unlink(local);
            return 1;
        }
        server_port = port;
        if (n > 4 && sys_write(out, pkt + 4, (unsigned long)(n - 4)) != n - 4) {
            fprintf(2, "tftp: cannot write %s (no space?)\n", local);
            sys_close(out);
            return 1;
        }
        total += (unsigned)(n - 4);
        unsigned char ack[4] = { 0, OP_ACK, (unsigned char)(block >> 8), (unsigned char)block };
        if (n - 4 < 512) {                       /* 最後一塊:確認之後結束 */
            sys_udp_send(sock, SERVER, server_port, ack, 4);
            break;
        }
        block++;
    }
    sys_close(out);
    sys_close(sock);
    printf("%s: %u bytes\n", local, total);
    return 0;
}
