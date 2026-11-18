/*
 * df.c - 顯示檔案系統的用量
 * 用法:df [路徑...]      沒有給路徑時顯示 / 與 /disk(有磁碟的話)
 *
 * 欄位:
 *   TYPE    檔案系統的種類:ramfs(記憶體,關機就消失)、wfs(磁碟)
 *   SIZE    可以放資料的總容量(KB)          USED / FREE  已使用 / 剩餘(KB)
 *   FILES   還可以再建立幾個檔案或目錄("-" = 沒有固定的上限)
 * ramfs 的容量就是整台機器的記憶體,所以 USED 包含 kernel 與所有行程用掉的部分。
 */
#include "ulib.h"

static void pad_num(struct line *l, long v, int width)
{
    int digits = 1;

    for (long t = v; t >= 10; t /= 10)
        digits++;
    for (; digits < width; digits++)
        line_str(l, " ");
    line_num(l, v);
}

static void pad_str(struct line *l, const char *s, int width)
{
    line_str(l, s);
    for (int n = (int)ustrlen(s); n < width; n++)
        line_str(l, " ");
}

/* 顯示一個路徑所在的檔案系統。quiet:查不到時不抱怨(預設清單裡的 /disk 可能不存在) */
static int show(const char *path, int quiet)
{
    struct fsinfo fi;
    struct line l = { .len = 0 };

    if (sys_statfs(path, &fi) != 0) {
        if (!quiet) {
            puts_("df: cannot read ");
            puts_(path);
            puts_("\n");
        }
        return 1;
    }
    long kb = (long)(fi.block_size / 1024);
    pad_str(&l, fi.type, 6);
    pad_num(&l, (long)fi.blocks * kb, 9);
    pad_num(&l, (long)(fi.blocks - fi.free_blocks) * kb, 9);
    pad_num(&l, (long)fi.free_blocks * kb, 9);
    if (fi.inodes)
        pad_num(&l, (long)fi.free_inodes, 7);
    else
        line_str(&l, "      -");
    line_str(&l, "  ");
    line_str(&l, path);
    line_str(&l, "\n");
    line_out(&l);
    return 0;
}

int main(int argc, char **argv)
{
    int rc = 0;

    puts_("TYPE    SIZE(KB) USED(KB) FREE(KB)  FILES  PATH\n");
    if (argc < 2) {
        show("/", 0);
        show("/disk", 1);
        return 0;
    }
    for (int i = 1; i < argc; i++)
        rc |= show(argv[i], 0);
    return rc;
}
