#ifndef WELYOS_UAPI_NET_H
#define WELYOS_UAPI_NET_H
/*
 * uapi/net.h - 網路:UDP 系統呼叫的參數、網路的設定與統計
 *
 * 這個目錄(uapi = user API)放的是 kernel 與使用者程式「共用」的定義:系統呼叫的編號、
 * 經由系統呼叫傳遞的結構、兩邊都要認得的常數。兩邊引入的是同一個檔案,
 * 所以不會再有「kernel 改了、使用者端忘了跟著改」的問題。
 * 規則:這裡的檔案只能引入 <stdint.h> / <stddef.h> 與同目錄的檔案,不能依賴 kernel 內部的任何東西;
 * 結構一律用固定寬度的型別,三個架構(32 與 64 位元)的佈局才會相同。
 * 已經公開的編號與欄位只增不改。
 */
#include <stdint.h>

/* IPv4 address as a host-order 32-bit integer; shared by kernel and user tools. */
#define NET_IP(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))


/* SYS_udp_send / SYS_udp_recv 的參數。
   送出:ip、port 是目的,data / len 是要送的資料。
   接收:data / len 是緩衝區與它的大小;返回時 ip、port 是來源,len 是實際的長度 */
struct udpmsg {
    uint32_t ip;
    uint16_t port, pad;
    uint32_t len, pad2;
    uint64_t data;              /* 使用者位址(固定用 64 位元,三個架構的格式才相同) */
};

/* 目前的設定與統計 */
struct netinfo {
    uint32_t up;                 /* 1 = 有網路卡而且已啟動 */
    uint8_t  mac[6];
    uint16_t pad;
    uint32_t ip, netmask, gateway;
    uint32_t rx_frames, tx_frames, rx_dropped;
    uint32_t rx_bad_length, rx_bad_checksum, rx_filtered, rx_unbound;
    uint32_t tx_errors, tx_queue_full, arp_timeouts, arp_cache_entries;
    uint32_t icmp_echo_replied, udp_delivered;
    uint32_t sock_dropped;       /* socket 的佇列滿了而丟掉的封包 */
};

/* 佈局的檢查:這些結構在 32 與 64 位元的架構上必須一樣大(欄位都對齊在自然的位置上) */
_Static_assert(sizeof(struct udpmsg) == 24, "ABI: struct udpmsg");
_Static_assert(sizeof(struct netinfo) == 80, "ABI: struct netinfo");
#endif
