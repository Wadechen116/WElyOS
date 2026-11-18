#ifndef WELYOS_IO_H
#define WELYOS_IO_H
#include <stdint.h>

/*
 * x86 有獨立的 I/O 位址空間(port 0~65535),需用專用的 in/out 指令存取,
 * 不能用一般指標。以下以 inline 組合語言包裝。
 */

/* 對 I/O port 寫入 1 位元組。"a" = 值放 al;"Nd" = port 為 8 位元常數或放 dx */
static inline void outb(uint16_t port, uint8_t v)
{
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

/* 從 I/O port 讀取 1 位元組。"=a" = 結果從 al 取出 */
static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* 從 I/O port 連續讀取 / 寫入 count 個 16 位元的字(ATA 硬碟的資料傳輸用)。
   rep insw / outsw:CPU 自己重複 count 次,位址放在 edi / esi */
static inline void insw(uint16_t port, void *buf, unsigned count)
{
    __asm__ volatile("cld; rep insw" : "+D"(buf), "+c"(count) : "d"(port) : "memory");
}

static inline void outsw(uint16_t port, const void *buf, unsigned count)
{
    __asm__ volatile("cld; rep outsw" : "+S"(buf), "+c"(count) : "d"(port) : "memory");
}
#endif
