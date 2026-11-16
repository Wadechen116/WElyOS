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
#endif
