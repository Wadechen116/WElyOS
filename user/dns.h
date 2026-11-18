#ifndef WELYOS_USER_DNS_H
#define WELYOS_USER_DNS_H
#include <stddef.h>
#include <stdint.h>

/* 網域名稱系統(Domain Name System, DNS)的純封包輔助函式。
   不依賴 libc、不配置記憶體、不做系統呼叫,可同時在使用者程式與主機測試使用。
   0:成功; -1:參數/格式/配對/伺服器錯誤; -2:沒有對應 A(包括 NXDOMAIN)。
   query 失敗時 *out_len=0; parse 失敗不改 *out_ip。IPv4 結果是主機順序。
   支援 ASCII 名稱、大小寫不敏感與單一結尾點;不做 IDNA、TCP fallback 或 DNSSEC。
   回應上限 65535 bytes、資源紀錄合計 128 筆、CNAME 8 跳、每名稱壓縮 32 跳。
   呼叫端仍須驗證 UDP 來源位址/埠、管理查詢 id 與逾時;這不是身分認證。 */
#define WELY_DNS_NAME 255u
#define WELY_DNS_RR_MAX 128u
static inline uint16_t wely_dns_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static inline void wely_dns_put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}
static inline uint8_t wely_dns_lower(uint8_t c)
{
    return c >= 'A' && c <= 'Z' ? (uint8_t)(c + ('a' - 'A')) : c;
}
static inline int wely_dns_equal(const uint8_t *a, size_t an, const uint8_t *b, size_t bn)
{
    if (an != bn) return 0;
    for (size_t i = 0; i < an; ++i) if (a[i] != b[i]) return 0;
    return 1;
}
static inline int wely_dns_encode(const char *name, uint8_t *out, size_t *length)
{
    if (!name || !out || !length) return -1;
    size_t n = 0;
    /* 即使呼叫者交來超長 C 字串,也不會掃描超過 DNS 名稱上限。 */
    for (; n <= 254u; ++n) if (!name[n]) break;
    if (!n || n > 254u) return -1;
    if (name[n - 1] == '.') --n;
    if (!n || n > 253u) return -1;
    size_t o = 0, start = 0;
    for (size_t i = 0; i <= n; ++i) {
        if (i < n && name[i] != '.') continue;
        size_t count = i - start;
        if (!count || count > 63u || o + 1u + count >= WELY_DNS_NAME) return -1;
        out[o++] = (uint8_t)count;
        for (size_t j = start; j < i; ++j) {
            uint8_t c = (uint8_t)name[j];
            if (c <= 32u || c >= 127u) return -1;
            out[o++] = wely_dns_lower(c);
        }
        start = i + 1u;
    }
    out[o++] = 0; *length = o; return 0;
}
static inline int dns_build_query(uint8_t *out, size_t cap, uint16_t id,
                                   const char *qname, size_t *out_len)
{
    uint8_t name[WELY_DNS_NAME]; size_t n;
    if (!out_len) return -1;
    *out_len = 0;
    if (!out || wely_dns_encode(qname, name, &n) || cap < 16u + n) return -1;
    for (size_t i = 0; i < 12u; ++i) out[i] = 0;
    wely_dns_put16(out, id); out[2] = 1; out[5] = 1; /* RD=1,QDCOUNT=1 */
    for (size_t i = 0; i < n; ++i) out[12u + i] = name[i];
    wely_dns_put16(out + 12u + n, 1); wely_dns_put16(out + 14u + n, 1);
    *out_len = 16u + n; return 0;
}
/* 名稱壓縮(name compression)的 pointer 只能指向較早的位置。
   加上步數與展開長度的硬上限,惡意循環或長鏈都不能拖住解析器。 */
static inline int wely_dns_name(const uint8_t *msg, size_t len, size_t *off,
                                uint8_t *out, size_t *out_len)
{
    size_t p = *off, end = p, o = 0;
    unsigned jumps = 0; int jumped = 0;
    for (unsigned step = 0; step < 256u; ++step) {
        if (p >= len) return -1;
        uint8_t count = msg[p];
        if ((count & 0xc0u) == 0xc0u) {
            if (len - p < 2u || ++jumps > 32u) return -1;
            size_t target = ((size_t)(count & 63u) << 8) | msg[p + 1];
            if (target < 12u || target >= p) return -1;
            if (!jumped) end = p + 2u;
            jumped = 1; p = target; continue;
        }
        if (count & 0xc0u) return -1;
        ++p;
        if (!count) {
            if (o >= WELY_DNS_NAME) return -1;
            out[o++] = 0;
            *off = jumped ? end : p; *out_len = o; return 0;
        }
        if (count > len - p || o + 1u + count >= WELY_DNS_NAME) return -1;
        out[o++] = count;
        for (unsigned i = 0; i < count; ++i) out[o++] = wely_dns_lower(msg[p + i]);
        p += count;
    }
    return -1;
}
struct wely_dns_record { uint16_t type, cls; size_t data, end; };
static inline int wely_dns_record_read(const uint8_t *msg, size_t len, size_t *off,
                                       uint8_t *owner, size_t *owner_len,
                                       struct wely_dns_record *rr)
{
    if (wely_dns_name(msg, len, off, owner, owner_len) || len - *off < 10u) return -1;
    const uint8_t *p = msg + *off;
    rr->type = wely_dns_u16(p); rr->cls = wely_dns_u16(p + 2);
    size_t n = wely_dns_u16(p + 8); rr->data = *off + 10u;
    if (n > len - rr->data) return -1;
    rr->end = rr->data + n; *off = rr->end;
    if (rr->cls == 1 && rr->type == 1 && n != 4u) return -1;
    if (rr->cls == 1 && rr->type == 5) {
        uint8_t target[WELY_DNS_NAME]; size_t target_len, at = rr->data;
        if (wely_dns_name(msg, len, &at, target, &target_len) || at != rr->end) return -1;
    }
    return 0;
}
/* 每一跳重新掃描有界的 answer 區,因此不需在小型使用者堆疊上配置 RR 陣列。
   必須先驗完整封包,不能找到第一個 A 就提早接受後面遭截斷的回應。 */
static inline int wely_dns_find_a(const uint8_t *msg, size_t len, size_t answers,
                                 unsigned count, uint8_t *current, size_t current_len,
                                 uint32_t *out_ip)
{
    for (unsigned hop = 0; hop <= 8u; ++hop) {
        uint8_t owner[WELY_DNS_NAME], next[WELY_DNS_NAME];
        size_t owner_len, next_len = 0, off = answers;
        int has_a = 0, has_cname = 0; uint32_t ip = 0;
        for (unsigned i = 0; i < count; ++i) {
            struct wely_dns_record rr;
            if (wely_dns_record_read(msg, len, &off, owner, &owner_len, &rr)) return -1;
            if (rr.cls != 1 || !wely_dns_equal(owner, owner_len, current, current_len)) continue;
            if (rr.type == 1 && !has_a) {
                const uint8_t *p = msg + rr.data;
                ip = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                     ((uint32_t)p[2] << 8) | p[3]; has_a = 1;
            } else if (rr.type == 5) {
                size_t at = rr.data, n;
                uint8_t target[WELY_DNS_NAME];
                if (wely_dns_name(msg, len, &at, target, &n)) return -1;
                if (has_cname && !wely_dns_equal(next, next_len, target, n)) return -1;
                for (size_t j = 0; j < n; ++j) next[j] = target[j];
                next_len = n; has_cname = 1;
            }
        }
        if (has_a && has_cname) return -1;
        if (has_a) { *out_ip = ip; return 0; }
        if (!has_cname) return -2;
        if (hop == 8u || wely_dns_equal(current, current_len, next, next_len)) return -1;
        for (size_t j = 0; j < next_len; ++j) current[j] = next[j];
        current_len = next_len;
    }
    return -1;
}
static inline int dns_parse_a(const uint8_t *msg, size_t len, uint16_t id,
                              const char *qname, uint32_t *out_ip)
{
    uint8_t expected[WELY_DNS_NAME], name[WELY_DNS_NAME];
    size_t expected_len, name_len, off = 12;
    if (!msg || !out_ip || len < 12u || len > 65535u ||
        wely_dns_encode(qname, expected, &expected_len)) return -1;
    uint16_t flags = wely_dns_u16(msg + 2);
    if (wely_dns_u16(msg) != id || !(flags & 0x8000u) ||
        (flags & 0x7a40u) || wely_dns_u16(msg + 4) != 1) return -1;
    /* opcode/TC/保留 Z 必須為零;AD/CD 可以存在,但不把它們當作驗證證據。 */
    unsigned rcode = flags & 15u;
    if (rcode != 0 && rcode != 3) return -1;
    if (wely_dns_name(msg, len, &off, name, &name_len) ||
        !wely_dns_equal(name, name_len, expected, expected_len) || len - off < 4u ||
        wely_dns_u16(msg + off) != 1 || wely_dns_u16(msg + off + 2) != 1) return -1;
    off += 4u; size_t answers = off;
    unsigned an = wely_dns_u16(msg + 6);
    unsigned all = an + (unsigned)wely_dns_u16(msg + 8) + (unsigned)wely_dns_u16(msg + 10);
    if (all > WELY_DNS_RR_MAX) return -1;
    for (unsigned i = 0; i < all; ++i) {
        struct wely_dns_record rr;
        if (wely_dns_record_read(msg, len, &off, name, &name_len, &rr)) return -1;
    }
    if (off != len) return -1;
    if (rcode == 3) return -2;
    return wely_dns_find_a(msg, len, answers, an, expected, expected_len, out_ip);
}
#endif
