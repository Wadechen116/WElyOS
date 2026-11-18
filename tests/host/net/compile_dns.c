#include "../../../user/dns.h"

/* 不只 include:讓三架構編譯器真的產生兩個公開函式的程式碼。 */
int compile_dns_query(uint8_t *out, size_t cap, uint16_t id, const char *name, size_t *len)
{
    return dns_build_query(out, cap, id, name, len);
}
int compile_dns_parse(const uint8_t *msg, size_t len, uint16_t id, const char *name, uint32_t *ip)
{
    return dns_parse_a(msg, len, id, name, ip);
}
