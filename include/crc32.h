#ifndef WELYOS_CRC32_H
#define WELYOS_CRC32_H
#include <stddef.h>
#include <stdint.h>

/*
 * CRC-32(Cyclic Redundancy Check,循環冗餘檢查):由一段資料算出的 32 位元檢查碼。
 * 資料只要有任何一個位元不同,算出來的值幾乎一定不同,所以把它和資料存在一起,
 * 讀回來時重算一次比對,就能發現儲存媒體上的損壞。
 * (它只能「發現」損壞,不能修復;也不是為了防止有人刻意竄改而設計的。)
 * 採用 IEEE 802.3 的多項式,和乙太網路、zip 用的相同。
 */
uint32_t crc32(const void *data, size_t len);
#endif
