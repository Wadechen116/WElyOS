/*
 * fdt.c - 極簡裝置樹(DTB)解析器
 *
 * RISC-V 與 AArch64 沒有 multiboot,bootloader 把硬體描述放在 DTB,
 * 位址由暫存器傳入(RISC-V: a1,AArch64: x0)。DTB 內所有整數為大端序。
 *
 * 結構:header | 記憶體保留表 | 結構區塊(token 流) | 字串區塊
 * token: 1=BEGIN_NODE(接節點名) 2=END_NODE 3=PROP(接 len、nameoff、資料)
 *        4=NOP 9=END
 *
 * 為了在 MMU 關閉(嚴格對齊)的環境安全,所有讀取都以位元組組合,不做未對齊存取。
 */
#include <stdint.h>
#include "fdt.h"
#include "kstring.h"

#define FDT_MAGIC       0xD00DFEEDu
#define FDT_BEGIN_NODE  1
#define FDT_END_NODE    2
#define FDT_PROP        3
#define FDT_NOP         4

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

/* 讀取由 cells 個 32 位元(大端)組成的數值(最多取 2 個 cell = 64 位元) */
static uint64_t be_cells(const uint8_t *p, uint32_t cells)
{
    uint64_t v = 0;
    for (uint32_t i = 0; i < cells; i++)
        v = (v << 32) | be32(p + 4 * i);
    return v;
}

int fdt_parse(const void *fdt, struct boot_info *bi)
{
    const uint8_t *base = fdt;

    bi->cmdline = "";
    bi->mem_base = 0;
    bi->mem_size = 0;
    bi->n_reserved = 0;
    if (!base || be32(base) != FDT_MAGIC)
        return -1;

    /* DTB 本身位於主記憶體內(cmdline 也指向它裡面),必須保留。
       header 偏移 4 的 totalsize 是整份 DTB 的大小 */
    boot_reserve(bi, (uintptr_t)base, be32(base + 4));

    /* 記憶體保留表(off_mem_rsvmap,偏移 16):一連串 64 位元的
       (位址, 大小),以 (0, 0) 結尾。bootloader 用它標示不可使用的範圍 */
    for (const uint8_t *r = base + be32(base + 16); ; r += 16) {
        uint64_t addr = be_cells(r, 2), size = be_cells(r + 8, 2);
        if (addr == 0 && size == 0)
            break;
        boot_reserve(bi, addr, size);
    }

    const uint8_t *p = base + be32(base + 8);            /* off_dt_struct */
    const char *strs = (const char *)base + be32(base + 12); /* off_dt_strings */

    uint32_t ac = 2, sc = 1;   /* 根節點 #address-cells / #size-cells 的規範預設值 */
    int depth = 0;             /* 根節點內為 1,根的子節點為 2 */
    enum { NONE, CHOSEN, MEMORY } cur = NONE;   /* 目前所在的深度 2 節點種類 */
    int got_mem = 0;

    for (;;) {
        uint32_t tok = be32(p);
        p += 4;

        if (tok == FDT_BEGIN_NODE) {
            const char *name = (const char *)p;
            depth++;
            cur = NONE;
            if (depth == 2) {
                if (!strcmp(name, "chosen"))
                    cur = CHOSEN;
                else if (!strncmp(name, "memory", 6))
                    cur = MEMORY;
            }
            p += (strlen(name) + 1 + 3) & ~3u;           /* 名稱以 4 位元組對齊 */
        } else if (tok == FDT_END_NODE) {
            depth--;
            cur = NONE;
        } else if (tok == FDT_PROP) {
            uint32_t len = be32(p);
            const char *pname = strs + be32(p + 4);
            const uint8_t *data = p + 8;
            p += 8 + ((len + 3) & ~3u);

            if (depth == 1) {
                if (!strcmp(pname, "#address-cells"))
                    ac = be32(data);
                else if (!strcmp(pname, "#size-cells"))
                    sc = be32(data);
            } else if (depth == 2 && cur == CHOSEN && !strcmp(pname, "bootargs")) {
                bi->cmdline = (const char *)data;        /* 以 NUL 結尾的字串 */
            } else if (depth == 2 && cur == MEMORY && !strcmp(pname, "reg") &&
                       !got_mem && len >= 4 * (ac + sc)) {
                bi->mem_base = be_cells(data, ac);
                bi->mem_size = be_cells(data + 4 * ac, sc);
                got_mem = 1;
            }
        } else if (tok != FDT_NOP) {
            break;                                       /* FDT_END 或無法辨識 */
        }
    }
    return 0;
}
