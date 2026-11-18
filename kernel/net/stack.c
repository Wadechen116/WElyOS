/* no-alloc-after-init */
#include "net.h"
#include "kstring.h"
#include "fs.h"
#include "wire.h"

/* 位址解析協定(Address Resolution Protocol, ARP)有兩個獨立的固定表:
   pending: 空閒 -> 等待(0/1/2/3 秒詢問) -> 回答後送出,或 4 秒逾時清空。
   cache: 空閒 -> 有效 -> 60 秒過期。未過期同 IP 不接受不同 MAC。
   一切狀態由外部共同鎖保護;send/回呼不得重入,故可共用靜態送出緩衝區。 */
#define SLOTS 8u
#define ARP_LIFE 60000u
struct cache_entry { uint32_t ip, since; uint8_t mac[6]; int used; };
struct pending_entry {
    uint32_t ip, since;
    unsigned attempts;
    size_t len;
    uint8_t frame[NET_FRAME_MAX];
    int used;
};
struct binding { uint16_t port; net_udp_fn fn; void *ctx; };
static struct netif *iface;
static struct cache_entry cache[SLOTS];
static struct pending_entry pending[SLOTS];
static struct binding bindings[SLOTS];
static struct net_stats stats;
static net_ping_fn ping_fn;
static void *ping_ctx;
static uint32_t clock_ms;
static uint16_t ip_id;
static uint8_t tx[NET_FRAME_MAX];
static const uint8_t broadcast[6] = {255,255,255,255,255,255};

enum drop_reason { BAD_LENGTH, BAD_CHECKSUM, FILTERED, UNBOUND };
static void drop(enum drop_reason why)
{
    ++stats.rx_dropped;
    switch (why) {
    case BAD_LENGTH: ++stats.rx_bad_length; break;
    case BAD_CHECKSUM: ++stats.rx_bad_checksum; break;
    case FILTERED: ++stats.rx_filtered; break;
    case UNBOUND: ++stats.rx_unbound; break;
    }
}
static int mac_unicast(const uint8_t *mac)
{
    static const uint8_t zero[6] = {0};
    return !(mac[0] & 1u) && memcmp(mac, zero, 6) != 0;
}
static int ip_unicast(uint32_t ip)
{
    if (!(ip >> 24) || (ip >> 24) == 127 || (ip >> 24) >= 224) return 0;
    /* /31 與 /32 沒有一般的子網路廣播位址。 */
    if (iface && ~iface->netmask > 1u &&
        ip == (iface->ip | ~iface->netmask)) return 0;
    return 1;
}
static int send_frame(const uint8_t *frame, size_t len)
{
    if (!iface || !iface->send || iface->send(iface, frame, len) < 0) {
        ++stats.tx_errors;
        return -E_IO;
    }
    ++stats.tx_frames;
    return 0;
}
static void eth_header(uint8_t *frame, const uint8_t *dst, uint16_t type)
{
    memcpy(frame, dst, 6); memcpy(frame + 6, iface->mac, 6);
    put16(frame + 12, type);
}
static int arp_send(uint16_t op, const uint8_t *mac, uint32_t ip)
{
    uint8_t frame[42];
    eth_header(frame, op == 1 ? broadcast : mac, 0x0806);
    put16(frame + 14, 1); put16(frame + 16, 0x0800);
    frame[18] = 6; frame[19] = 4; put16(frame + 20, op);
    memcpy(frame + 22, iface->mac, 6); put32(frame + 28, iface->ip);
    if (op == 1) memset(frame + 32, 0, 6);
    else memcpy(frame + 32, mac, 6);
    put32(frame + 38, ip);
    int rc = send_frame(frame, sizeof(frame));
    if (!rc) {
        if (op == 1) ++stats.arp_requests_sent;
        else ++stats.arp_replies_sent;
    }
    return rc;
}
static void expire_cache(void)
{
    for (unsigned i = 0; i < SLOTS; ++i) {
        if (cache[i].used && (uint32_t)(clock_ms - cache[i].since) >= ARP_LIFE) {
            cache[i].used = 0; --stats.arp_cache_entries;
        }
    }
}
static struct cache_entry *cache_find(uint32_t ip)
{
    expire_cache();
    for (unsigned i = 0; i < SLOTS; ++i)
        if (cache[i].used && cache[i].ip == ip) return &cache[i];
    return NULL;
}
static struct pending_entry *pending_find(uint32_t ip)
{
    for (unsigned i = 0; i < SLOTS; ++i)
        if (pending[i].used && pending[i].ip == ip) return &pending[i];
    return NULL;
}
static int learn(uint32_t ip, const uint8_t *mac)
{
    struct cache_entry *e = cache_find(ip);
    if (e && memcmp(e->mac, mac, 6)) return -1;
    if (!e) {
        unsigned slot = 0;
        for (unsigned i = 0; i < SLOTS; ++i) {
            if (!cache[i].used) { slot = i; break; }
            if ((uint32_t)(clock_ms - cache[i].since) >
                (uint32_t)(clock_ms - cache[slot].since)) slot = i;
        }
        e = &cache[slot];
        if (!e->used) ++stats.arp_cache_entries;
    }
    e->used = 1; e->ip = ip; e->since = clock_ms; memcpy(e->mac, mac, 6);
    struct pending_entry *p = pending_find(ip);
    if (p) {
        memcpy(p->frame, mac, 6);
        /* 封包送交驅動後不自動重送,避免 UDP 重複傳送的語意不清。 */
        (void)send_frame(p->frame, p->len); p->used = 0;
    }
    return 0;
}
static void arp_rx(const uint8_t *f, size_t len)
{
    if (len < 42) { drop(BAD_LENGTH); return; }
    if (wire16(f + 14) != 1 || wire16(f + 16) != 0x0800 ||
        f[18] != 6 || f[19] != 4) { drop(FILTERED); return; }
    uint16_t op = wire16(f + 20);
    uint32_t src = wire32(f + 28), dst = wire32(f + 38);
    if ((op != 1 && op != 2) || !iface->ip || dst != iface->ip ||
        memcmp(f + 6, f + 22, 6) || !mac_unicast(f + 22) ||
        (src && (!ip_unicast(src) || src == iface->ip))) {
        drop(FILTERED); return;
    }
    if (op == 2 && (!src || !pending_find(src) ||
        memcmp(f + 32, iface->mac, 6) || memcmp(f, iface->mac, 6))) {
        drop(FILTERED); return;
    }
    /* ARP probe 的來源 IP 為零,可以回答但不能污染快取。 */
    if (src && learn(src, f + 22) < 0) { drop(FILTERED); return; }
    if (op == 1) (void)arp_send(2, f + 22, src);
}
static uint32_t udp_sum(uint32_t src, uint32_t dst, const uint8_t *udp, size_t len)
{
    return (src >> 16) + (src & 0xffffu) + (dst >> 16) + (dst & 0xffffu) +
           17u + (uint32_t)len + wire_sum(udp, len);
}
static void ip_header(uint32_t dst, uint8_t proto, size_t payload)
{
    uint8_t *ip = tx + 14;
    memset(ip, 0, 20); ip[0] = 0x45;
    put16(ip + 2, (uint16_t)(20 + payload)); put16(ip + 4, ip_id++);
    put16(ip + 6, 0x4000); ip[8] = 64; ip[9] = proto;
    put32(ip + 12, iface->ip); put32(ip + 16, dst);
    put16(ip + 10, wire_finish(wire_sum(ip, 20)));
    eth_header(tx, broadcast, 0x0800);
}
static int route(uint32_t dst, size_t len)
{
    uint32_t hop = ((dst & iface->netmask) == (iface->ip & iface->netmask)) ?
                   dst : iface->gateway;
    if (!ip_unicast(hop) || hop == iface->ip) return -E_INVAL;
    struct cache_entry *e = cache_find(hop);
    if (e) { memcpy(tx, e->mac, 6); return send_frame(tx, len); }
    if (pending_find(hop)) { ++stats.tx_queue_full; return -E_BUSY; }
    for (unsigned i = 0; i < SLOTS; ++i) {
        struct pending_entry *p = &pending[i];
        if (p->used) continue;
        p->ip = hop; p->since = clock_ms; p->attempts = 1;
        p->len = len; memcpy(p->frame, tx, len); p->used = 1;
        int rc = arp_send(1, NULL, hop);
        if (rc < 0) p->used = 0;
        return rc;
    }
    ++stats.tx_queue_full;
    return -E_NOMEM;
}
static void icmp_rx(const uint8_t *f, const uint8_t *p, size_t n, uint32_t src)
{
    if (n < 8) { drop(BAD_LENGTH); return; }
    if (wire_finish(wire_sum(p, n))) { drop(BAD_CHECKSUM); return; }
    if (p[1] || (p[0] != 0 && p[0] != 8)) { drop(FILTERED); return; }
    if (!p[0]) {
        if (ping_fn) ping_fn(ping_ctx, src, wire16(p + 4), wire16(p + 6), p + 8, n - 8);
        return;
    }
    ip_header(src, 1, n); memcpy(tx + 34, p, n);
    tx[34] = 0; put16(tx + 36, 0);
    put16(tx + 36, wire_finish(wire_sum(tx + 34, n)));
    /* 回答僅送給驗證過的入站單播來源,不需新增無界的解析工作。 */
    memcpy(tx, f + 6, 6);
    if (!send_frame(tx, 34 + n)) ++stats.icmp_echo_replied;
}
static void udp_rx(const uint8_t *p, size_t n, uint32_t src, uint32_t dst)
{
    if (n < 8 || wire16(p + 4) != n) { drop(BAD_LENGTH); return; }
    if (wire16(p + 6) && wire_finish(udp_sum(src, dst, p, n))) {
        drop(BAD_CHECKSUM); return;
    }
    uint16_t port = wire16(p + 2);
    for (unsigned i = 0; i < SLOTS; ++i) {
        if (bindings[i].fn && bindings[i].port == port) {
            ++stats.udp_delivered;
            bindings[i].fn(bindings[i].ctx, src, wire16(p), p + 8, n - 8);
            return;
        }
    }
    drop(UNBOUND);
}
static void ipv4_rx(const uint8_t *f, size_t len)
{
    if (len < 34) { drop(BAD_LENGTH); return; }
    const uint8_t *p = f + 14;
    unsigned ihl = (p[0] & 15u) * 4u;
    size_t total = wire16(p + 2);
    if (ihl < 20 || total < ihl || total > NET_MTU || total > len - 14) {
        drop(BAD_LENGTH); return;
    }
    if ((p[0] >> 4) != 4 || ihl != 20) { drop(FILTERED); return; }
    if (wire_finish(wire_sum(p, ihl))) { drop(BAD_CHECKSUM); return; }
    uint32_t src = wire32(p + 12), dst = wire32(p + 16);
    if (!iface->ip || !ip_unicast(src) || dst != iface->ip || !ip_unicast(dst) ||
        memcmp(f, iface->mac, 6) || !p[8] || (wire16(p + 6) & 0xbfffu)) {
        drop(FILTERED); return;
    }
    if (p[9] == 1) icmp_rx(f, p + 20, total - 20, src);
    else if (p[9] == 17) udp_rx(p + 20, total - 20, src, dst);
    else drop(FILTERED);
}
void net_init(struct netif *nif)
{
    iface = nif; clock_ms = 0; ip_id = 0; ping_fn = NULL; ping_ctx = NULL;
    memset(cache, 0, sizeof(cache)); memset(pending, 0, sizeof(pending));
    memset(bindings, 0, sizeof(bindings)); memset(&stats, 0, sizeof(stats));
}
void net_rx(struct netif *nif, const void *frame, size_t len)
{
    ++stats.rx_frames;
    if (!frame || len < 14 || len > NET_FRAME_MAX) { drop(BAD_LENGTH); return; }
    if (!iface || nif != iface) { drop(FILTERED); return; }
    const uint8_t *f = frame;
    if (!mac_unicast(f + 6) ||
        (memcmp(f, iface->mac, 6) && memcmp(f, broadcast, 6))) {
        drop(FILTERED); return;
    }
    if (wire16(f + 12) == 0x0806) arp_rx(f, len);
    else if (wire16(f + 12) == 0x0800) ipv4_rx(f, len);
    else drop(FILTERED);
}
void net_tick(uint32_t now_ms)
{
    clock_ms = now_ms; expire_cache();
    if (!iface) return;
    for (unsigned i = 0; i < SLOTS; ++i) {
        struct pending_entry *p = &pending[i];
        if (!p->used) continue;
        uint32_t age = (uint32_t)(clock_ms - p->since);
        if (age >= 4000u) { p->used = 0; ++stats.arp_timeouts; }
        else if (p->attempts < 4 && age >= p->attempts * 1000u) {
            ++p->attempts;
            (void)arp_send(1, NULL, p->ip);
        }
    }
}
static int send_valid(uint32_t dst, const void *data, size_t len, size_t max)
{
    return iface && iface->send && iface->ip && ip_unicast(iface->ip) &&
           mac_unicast(iface->mac) && ip_unicast(dst) && dst != iface->ip &&
           len <= max && (!len || data);
}
int net_ping_send(uint32_t dst, uint16_t id, uint16_t seq, const void *data, size_t len)
{
    if (!send_valid(dst, data, len, NET_PING_MAX)) return -E_INVAL;
    ip_header(dst, 1, 8 + len);
    uint8_t *p = tx + 34;
    p[0] = 8; p[1] = 0; put16(p + 2, 0); put16(p + 4, id); put16(p + 6, seq);
    if (len) memcpy(p + 8, data, len);
    put16(p + 2, wire_finish(wire_sum(p, 8 + len)));
    return route(dst, 42 + len);
}
void net_ping_handler(net_ping_fn fn, void *ctx)
{
    ping_fn = fn; ping_ctx = ctx;
}
int net_udp_bind(uint16_t port, net_udp_fn fn, void *ctx)
{
    if (!port || !fn) return -E_INVAL;
    for (unsigned i = 0; i < SLOTS; ++i)
        if (bindings[i].fn && bindings[i].port == port) return -E_BUSY;
    for (unsigned i = 0; i < SLOTS; ++i) {
        if (!bindings[i].fn) {
            bindings[i].port = port; bindings[i].fn = fn; bindings[i].ctx = ctx;
            return 0;
        }
    }
    return -E_NOMEM;
}
void net_udp_unbind(uint16_t port)
{
    for (unsigned i = 0; i < SLOTS; ++i)
        if (bindings[i].fn && bindings[i].port == port) bindings[i].fn = NULL;
}
int net_udp_send(uint16_t src_port, uint32_t dst, uint16_t dst_port,
                 const void *data, size_t len)
{
    if (!dst_port || !send_valid(dst, data, len, NET_UDP_MAX)) return -E_INVAL;
    ip_header(dst, 17, 8 + len);
    uint8_t *p = tx + 34;
    put16(p, src_port); put16(p + 2, dst_port); put16(p + 4, (uint16_t)(8 + len));
    put16(p + 6, 0); if (len) memcpy(p + 8, data, len);
    uint16_t sum = wire_finish(udp_sum(iface->ip, dst, p, 8 + len));
    put16(p + 6, sum ? sum : 0xffffu);
    return route(dst, 42 + len);
}
void net_get_stats(struct net_stats *out)
{
    expire_cache();
    if (out) *out = stats;
}
