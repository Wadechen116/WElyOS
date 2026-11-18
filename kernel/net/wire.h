#ifndef WELYOS_NET_WIRE_H
#define WELYOS_NET_WIRE_H
#include <stddef.h>
#include <stdint.h>

/* 逐位元組存取避免未對齊存取,也不依賴主機的位元組順序。 */
static inline uint16_t wire16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static inline uint32_t wire32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static inline void put16(uint8_t *p, uint16_t x)
{
    p[0] = (uint8_t)(x >> 8); p[1] = (uint8_t)x;
}
static inline void put32(uint8_t *p, uint32_t x)
{
    p[0] = (uint8_t)(x >> 24); p[1] = (uint8_t)(x >> 16);
    p[2] = (uint8_t)(x >> 8); p[3] = (uint8_t)x;
}
/* 網際網路檢查碼(Internet checksum),呼叫者把長度限於一個 MTU,
   因此 32 位元加總不會溢位;奇數尾端以零補齊。 */
static inline uint32_t wire_sum(const uint8_t *p, size_t n)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < n / 2; ++i) sum += wire16(p + 2 * i);
    if (n & 1u) sum += (uint32_t)p[n - 1] << 8;
    return sum;
}
static inline uint16_t wire_finish(uint32_t sum)
{
    sum = (sum & 0xffffu) + (sum >> 16);
    sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)~sum;
}
#endif
