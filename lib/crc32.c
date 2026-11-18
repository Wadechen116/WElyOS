/*
 * crc32.c - CRC-32(說明見 crc32.h)
 *
 * 查表法:先算好「一個位元組的 256 種可能」各自對檢查碼的影響,之後每個位元組查一次表,
 * 比逐位元計算快八倍左右。檔案系統對每個讀寫的區塊(4KB)都要算一次,所以值得。
 * 表格在第一次使用時建立(1KB)。
 */
#include "crc32.h"

static uint32_t table[256];
static volatile int table_ready;

static void build_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int b = 0; b < 8; b++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
        table[i] = c;
    }
    table_ready = 1;        /* 兩顆 CPU 同時建立也無妨:寫進去的是完全相同的內容 */
}

uint32_t crc32(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t crc = 0xFFFFFFFFu;

    if (!table_ready)
        build_table();
    for (size_t i = 0; i < len; i++)
        crc = (crc >> 8) ^ table[(crc ^ p[i]) & 0xFF];
    return ~crc;
}
