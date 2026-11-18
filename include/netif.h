#ifndef WELYOS_NETIF_H
#define WELYOS_NETIF_H
#include <stddef.h>
#include <stdint.h>
#include "uapi/net.h"         /* struct netinfo、struct udpmsg */

/*
 * netif.h - 把網路卡驅動程式(netdev.h)與協定堆疊(net.h)接進 kernel 的那一層。
 *
 * 協定堆疊本身不上鎖、不知道執行緒與行程(見 net.h 的規則)。這一層負責:
 *   - 一把鎖:同一時間只有一條執行緒在堆疊裡面
 *   - 一條 kernel 執行緒:定期向驅動程式要收到的訊框交給堆疊,並推動堆疊的計時
 *   - 把「送出後等回答」做成會等待的函式,給系統呼叫使用
 *   - UDP 的 socket:以檔案描述符的形式交給行程(關閉描述符或行程結束時自動釋放)
 * 位址與埠號都是主機順序(10.0.2.15 = 0x0A00020F)。
 */
struct netdev;
struct file;

/* 以 dev 啟動網路(dev 為 NULL = 沒有網路卡:之後的呼叫都回傳 -E_NETDOWN) */
void netif_start(struct netdev *dev);

/* ping:送出一個 echo request,等回答。回傳來回花了幾毫秒;逾時回傳 -E_TIMEDOUT */
long netif_ping(uint32_t ip, unsigned timeout_ms);

/* 建立一個綁定在本機 port 的 UDP socket(port 為 0 = 由系統挑一個沒人用的)。
   1024 以下的埠需要 CAP_ADMIN。成功時 *out 是一次「開啟」,可以放進描述符表 */
int  netif_udp_open(uint16_t port, struct file **out);
/* 送出一個封包;回傳送出的長度 */
long netif_udp_send(struct file *f, uint32_t ip, uint16_t port, const void *data, size_t len);
/* 等一個封包(最多 timeout_ms 毫秒;0 = 不等)。回傳長度,並填入來源;沒有回傳 -E_TIMEDOUT。
   比 cap 長的封包被截斷 */
long netif_udp_recv(struct file *f, uint32_t *ip, uint16_t *port, void *buf, size_t cap,
                    unsigned timeout_ms);

void netif_info(struct netinfo *out);
#endif
