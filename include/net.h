#ifndef WELYOS_NET_H
#define WELYOS_NET_H
#include <stddef.h>
#include <stdint.h>
#include "uapi/net.h"

/*
 * net.h - 網路:驅動程式、協定堆疊、kernel 其他部分三者之間的介面
 *
 * 分成三塊,各自只認得這個檔案:
 *
 *   kernel 其他部分(系統呼叫、ping 之類的使用者程式)
 *        |  net_ping_send / net_udp_send / net_udp_bind ...
 *        v
 *   協定堆疊(kernel/net/ 底下)   乙太網路、ARP、IPv4、ICMP、UDP
 *        |  nif->send(整個乙太網路訊框)        ^ net_rx(收到的訊框)
 *        v                                      |
 *   網路卡驅動程式                 virtio-net 等
 *
 * 協定堆疊不碰硬體、不配置記憶體、不上鎖、不知道行程是什麼,所以同一份程式
 * 可以在主機上直接編譯測試(tests/host/net)。
 *
 * ---- 規則(三方都要遵守) ----
 *
 * 1. 位元組順序:這個檔案裡所有的 IPv4 位址與埠號都是「主機順序」的整數
 *    (10.0.2.15 = 0x0A00020F)。只有堆疊內部在讀寫封包時轉成網路順序。
 * 2. 上鎖:堆疊本身不上鎖。呼叫者保證同一時間只有一條執行緒在 net_* 裡面
 *    (整合層用一把 mutex 包住所有進入點,包括 net_rx 與 net_tick)。
 *    堆疊在 net_* 裡面呼叫 nif->send 與各個回呼(callback);回呼裡不可以再呼叫 net_*。
 * 3. 記憶體:堆疊不呼叫 kmalloc / pmm。所有表格是固定大小的靜態陣列。
 *    傳進來的指標(訊框、資料)只在那一次呼叫期間有效,要留的東西自己複製。
 * 4. 不信任收到的內容:net_rx 的訊框來自網路,任何長度欄位、位移都要先檢查,
 *    不合理的封包直接丟掉,絕不可以讀寫到 len 之外。
 * 5. 堆疊只可以使用 kstring.h 的函式(memcpy、memset、memcmp、memmove、strlen);
 *    不可以使用 64 位元的除法與餘數(i386 沒有連結 libgcc)、浮點數、可變長度陣列。
 * 6. 回傳值:0 或正數表示成功,負值是 fs.h 的 -E_*(參數錯誤 -E_INVAL、
 *    沒有空位 -E_NOMEM / -E_BUSY、送不出去 -E_IO)。
 * 7. 生命期:nif 這個物件必須一直存在到下一次 net_init 為止;它的欄位(位址設定)
 *    只在同一把鎖之下更改。交給回呼的資料指標只在那一次回呼期間有效。
 * 8. 時間:net_tick 的 now_ms 是會繞回(wrap)的 32 位元毫秒數;堆疊一律以「模 2^32 的差」
 *    計算經過的時間。整合層保證它單調遞增,而且相鄰兩次呼叫相隔遠小於 2^31 毫秒。
 *    net_init 之後、第一次送出任何東西之前,整合層先呼叫一次 net_tick(now),
 *    讓堆疊知道「現在」是多少(否則第一次 tick 的大數值會讓剛排隊的東西立刻過期)。
 * 9. 安全策略(堆疊必須遵守,測試要涵蓋):
 *    - ARP:不論是詢問還是回答,都不可以用不同的 MAC 覆蓋「還沒過期」的既有紀錄;
 *      乙太網路標頭的來源 MAC 要和 ARP 內的送出者 MAC 一致;只接受和等待中的詢問相符的回答。
 *      (這只是降低被欺騙的機會,不是身分認證。)
 *    - IPv4:有選項(標頭長度不是 20)的、被分段的(MF 位元或位移不為 0)一律丟掉;
 *      廣播與群播位址的 echo request 不回答。
 *    - UDP:收到檢查碼欄位為 0 的封包是合法的(對方沒有算);送出一律要算,
 *      算出來是 0 時依規定寫成 0xFFFF。
 */

#define NET_MTU        1500u             /* 乙太網路訊框裡承載資料的上限 */
#define NET_FRAME_MAX  1514u             /* 訊框最大長度(14 位元組標頭 + 1500;不含硬體自己加的檢查碼) */
#define NET_UDP_MAX    1472u             /* 一個 UDP 封包最多帶多少資料(不做 IP 分段) */
#define NET_PING_MAX   1472u             /* ICMP echo 最多帶多少資料 */


/*
 * 一個網路介面。由驅動程式(或測試)填好之後交給 net_init。
 *   send    送出一個完整的乙太網路訊框(從目的 MAC 開始,長度 14 ~ NET_FRAME_MAX)。
 *           不滿 60 位元組的由驅動程式或硬體自己補齊。0 = 已交給硬體,負值 = 失敗。
 *           驅動程式必須在返回之前把訊框用完或複製走:返回之後那塊記憶體就不再有效
 *           (堆疊用的是暫存的緩衝區)。send 是在 net 的鎖裡面被呼叫的,
 *           所以它不可以等待任何「需要那把鎖才能完成」的事
 *   driver  驅動程式自己的資料;堆疊不碰
 */
struct netif {
    uint8_t  mac[6];
    uint32_t ip;                         /* 這台機器的位址;0 = 還沒設定(此時只處理 ARP 以外都丟掉) */
    uint32_t netmask;
    uint32_t gateway;                    /* 不在同一個子網路的封包送給它;0 = 沒有 */
    int    (*send)(struct netif *nif, const void *frame, size_t len);
    void    *driver;
};

/* ---- 協定堆疊提供(kernel/net/ 底下的 .c) ---- */

/* 以 nif 作為唯一的網路介面,清空所有表格。可以重複呼叫(測試會這樣做) */
void net_init(struct netif *nif);

/* 驅動程式收到一個訊框時呼叫(在執行緒裡,不是中斷處理程式)。len 是實際收到的長度 */
void net_rx(struct netif *nif, const void *frame, size_t len);

/* 定期呼叫(大約每 100 毫秒),now_ms 是遞增的毫秒數:讓 ARP 的紀錄過期、重送等待中的 ARP 詢問 */
void net_tick(uint32_t now_ms);

/*
 * ICMP echo(ping)。
 *   net_ping_send   送出一個 echo request。目的位址的 MAC 還不知道時,堆疊先送 ARP 詢問,
 *                   並把這一個封包留著,等回答到了再送出;這種情況也回傳 0
 *                   (0 表示「已接受」,不保證已經送上網路)。
 *                   等待中的封包以「下一站」的位址為單位,每個下一站留一個
 *                   (經過閘道的不同目的地共用閘道那一格):
 *                     那一格已經有封包在等 -> -E_BUSY(不可以悄悄蓋掉先前已回報成功的那一個)
 *                     等待的表格滿了        -> -E_NOMEM
 *                   ARP 詢問的時程:第 0、1、2、3 秒各送一次(第一次加三次重試),
 *                   第 4 秒仍沒有回答就放棄,丟掉留著的封包,並記入 arp_timeouts。
 *   net_ping_handler 設定「收到 echo reply」時要呼叫的函式(NULL = 不通知)。
 *                   別人 ping 我們的 echo request 由堆疊自己回答,不經過這裡。
 */
int  net_ping_send(uint32_t dst_ip, uint16_t id, uint16_t seq, const void *data, size_t len);
typedef void (*net_ping_fn)(void *ctx, uint32_t src_ip, uint16_t id, uint16_t seq,
                            const void *data, size_t len);
void net_ping_handler(net_ping_fn fn, void *ctx);

/*
 * UDP。
 *   net_udp_bind    之後送到本機 port 的封包都交給 fn。port 已經被綁定回傳 -E_BUSY;
 *                   表格滿了回傳 -E_NOMEM。至少支援 8 個同時的綁定
 *   net_udp_unbind  取消
 *   net_udp_send    送出一個封包(同樣可能先做 ARP,見 net_ping_send)。
 *                   len 超過 NET_UDP_MAX 回傳 -E_INVAL。檢查碼一律要算
 */
typedef void (*net_udp_fn)(void *ctx, uint32_t src_ip, uint16_t src_port,
                           const void *data, size_t len);
int  net_udp_bind(uint16_t port, net_udp_fn fn, void *ctx);
void net_udp_unbind(uint16_t port);
int  net_udp_send(uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, const void *data, size_t len);

/* 統計(除錯與測試用);堆疊自己維護 */
struct net_stats {
    uint32_t rx_frames, tx_frames;
    uint32_t rx_dropped;                 /* 格式不對、檢查碼錯、不是給我們的、沒有人收 */
    uint32_t arp_requests_sent, arp_replies_sent, arp_cache_entries;
    uint32_t icmp_echo_replied;          /* 回答了幾個別人的 ping */
    uint32_t udp_delivered;
    /* 以下是 rx_dropped 的細分:每個被丟掉的訊框只記在「最主要的那一個原因」上,
       所以這四項的總和等於 rx_dropped */
    uint32_t rx_bad_length;              /* 太短、長度欄位彼此矛盾、超出實際收到的長度 */
    uint32_t rx_bad_checksum;
    uint32_t rx_filtered;                /* 格式正確但不處理:不是給我們的、不支援的協定或選項、被安全策略拒絕 */
    uint32_t rx_unbound;                 /* UDP:那個埠沒有人綁定 */
    uint32_t tx_errors;                  /* nif->send 回報失敗的次數 */
    uint32_t tx_queue_full;              /* 因為等待 ARP 的位置被佔用或表格已滿而拒絕(-E_BUSY / -E_NOMEM) */
    uint32_t arp_timeouts;               /* 問了四次都沒有回答而放棄的次數 */
};
void net_get_stats(struct net_stats *out);
#endif
