/*
 * dir_error_probe.c - WFS「讀不到」不可以被當成「沒有」(第 50 步,D4 / ISSUES N-006)
 *
 * 真實的區塊快取(bcache.c)加上從 kernel/wfs.c 摘取的真函式;磁碟換成記憶體裡的映像,
 * 可以指定某個區塊讀取失敗。每個情境都核對「回報的結果」:
 *   balloc-read     位元圖讀不到        -> 必須回報 -E_IO,不是「磁碟滿了」
 *   ialloc-read     inode 表讀不到      -> 必須回報 -E_IO,不是「沒有空的 inode」
 *   empty-read      目錄的資料讀不到    -> is_empty 必須回報 -E_IO,不是「不是空的」
 *   readdir-inode   某一筆指向的 inode 讀不到 -> readdir 回報 -E_IO 並交出名稱,位置往後移
 *   check-inode     檢查程式讀不到某一筆指向的 inode -> 停下來,不可以把那筆名稱清掉
 *   *-ok            同樣的操作在沒有錯誤時的正常結果(對照)
 * 摘取的工具會依原始碼的介面定義 OLD_* 巨集,讓修正前的程式也能編譯(負對照用);
 * 那時以修正前的呼叫者對回傳值的解讀來判定。
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "bcache.h"
#include "blk.h"
#include "mmu.h"
#include "uapi/errno.h"
#define S_PERM_MASK 0777u
#define FS_NAME_MAX 59
enum { I_FILE = 1, I_DIR, I_DEV };
struct dirent { char name[FS_NAME_MAX + 1]; uint32_t type; };
struct inode { uint64_t size, mtime; unsigned mode, uid, gid, nlink; };
struct mutex { int held; };
static void mutex_lock(struct mutex *m) { if (m->held) abort(); m->held = 1; }
static void mutex_unlock(struct mutex *m) { if (!m->held) abort(); m->held = 0; }
static int wfs_ops;
#define inode_init(in, ops, type) ((void)(in), (void)(ops), (void)(type))
static uint64_t timer_epoch(void *p) { (void)p; return 1000; }
#include "wfs_types.h"
#define BLOCKS 160u
static uint8_t image[BLOCKS][BSIZE];
static struct wsuper sb;
static struct mutex wfs_lk;
static struct wnode node_tab[128], *nodes = node_tab;    /* 至少 sb.ninodes 格 */
static uint32_t free_blocks, free_inodes, balloc_hint, ialloc_hint;
static int fs_failed, fail_read = -1;
static int covered(uint32_t no) { (void)no; return 0; }
static void csum_set(uint32_t no, uint32_t crc) { (void)no; (void)crc; }
static void bcache_forget_stub(uint32_t no) { (void)no; }
void *pmm_alloc_pages(size_t pages) { return calloc(pages, PAGE_SIZE); }
void pmm_free_pages(void *p, size_t pages) { (void)pages; free(p); }
_Noreturn void panic(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); abort(); }
static int ram_rw(struct blkdev *dev, uint64_t lba, unsigned count, void *buf, int write)
{
    (void)dev;
    if (lba % BSECT || count != BSECT || lba / BSECT >= BLOCKS) abort();
    unsigned no = (unsigned)(lba / BSECT);
    if (write) { memcpy(image[no], buf, BSIZE); return 0; }
    if ((int)no == fail_read) return -E_IO;
    memcpy(buf, image[no], BSIZE);
    return 0;
}
static void write_failed(uint32_t no) { (void)no; fs_failed = 1; }
static const struct bcache_ops callbacks = { .write_failed = write_failed };
static struct blkdev device = { .name = "ram", .sectors = BLOCKS * BSECT, .rw = ram_rw };
#include "wfs_functions.c"

/* 版面:位元圖 1、inode 表 2..7、資料從 8 起。目錄 inode 3 的資料在區塊 30,
   裡面一筆 "file" 指向 inode 70 號 — 放在 inode 表的第 2 個區塊(區塊 3),讓它可以單獨讀不到 */
#define DIR_INUM  3u
#define FILE_INUM (unsigned)(IPB + 5)
static void setup(void)
{
    sb = (struct wsuper){ .nblocks = BLOCKS, .ninodes = 128,
                          .bitmap_start = 1, .inode_start = 2, .data_start = 8 };
    free_blocks = 100; free_inodes = 10;
    for (unsigned no = 0; no < 8; no++) image[1][no / 8] |= (uint8_t)(1u << (no % 8));
    image[1][30 / 8] |= (uint8_t)(1u << (30 % 8));
    struct dinode *it = (struct dinode *)image[2];
    it[DIR_INUM] = (struct dinode){ .type = I_DIR, .nlink = 1, .size = BSIZE };
    it[DIR_INUM].addr[0] = 30;
    struct dinode *it2 = (struct dinode *)image[2 + FILE_INUM / IPB];
    it2[FILE_INUM % IPB] = (struct dinode){ .type = I_FILE, .nlink = 1 };
    struct wdirent *de = (struct wdirent *)image[30];
    de[0].inum = FILE_INUM;
    strcpy(de[0].name, "file");
    if (bcache_init(&device, &sb.nblocks, &callbacks)) abort();
}

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "line %d: %s\n", __LINE__, #expr); errors++; } } while (0)
int main(int argc, char **argv)
{
    if (argc != 2) return 90;
    const char *kind = argv[1];
    int errors = 0, ok = strstr(kind, "-ok") != NULL;
    long rc = 0;
    setup();
    if (!strncmp(kind, "balloc", 6)) {
        if (!ok) fail_read = 1;
        int err = 0;
#ifdef OLD_BALLOC
        uint32_t no = balloc();
#else
        uint32_t no = balloc(&err);
#endif
        rc = no ? (long)no : err;
        if (ok) CHECK(no >= 8 && no != 30);
        else CHECK(no == 0 && err == -E_IO);     /* 舊的程式:err 沒有被設定 = 呼叫者當成「磁碟滿了」 */
    } else if (!strncmp(kind, "ialloc", 6)) {
        if (!ok) fail_read = 2;                  /* inode 表的第一個區塊 */
        int err = -E_NOSPC;
#ifdef OLD_IALLOC
        struct wnode *n = ialloc(I_FILE, DIR_INUM);  /* 舊的呼叫者(wfs_create)把 NULL 一律當成 -E_NOSPC */
#else
        struct wnode *n = ialloc(I_FILE, DIR_INUM, &err);
#endif
        rc = n ? (long)n->inum : err;
        if (ok) CHECK(n != NULL);
        else CHECK(n == NULL && err == -E_IO);
    } else if (!strncmp(kind, "empty", 5)) {
        if (!ok) fail_read = 30;
        struct wnode *d = nget(DIR_INUM);
        CHECK(d != NULL);
        rc = d ? wfs_is_empty(&d->in) : -999;
        CHECK(rc == (ok ? 0 : -E_IO));           /* 舊的程式回傳 0(不是空的) */
    } else if (!strncmp(kind, "readdir", 7)) {
        struct wnode *d = nget(DIR_INUM);
        CHECK(d != NULL);
        if (!ok) fail_read = (int)(2 + FILE_INUM / IPB);
        struct dirent out;
        uint64_t pos = 0;
        memset(&out, 0, sizeof(out));
        rc = d ? wfs_readdir(&d->in, &pos, &out) : -999;
        CHECK(!strcmp(out.name, "file") && pos == 1);
        if (ok) CHECK(rc == 0 && out.type == I_FILE);
        else CHECK(rc == -E_IO);                 /* 舊的程式:rc 0、類型 0,看不出出了錯 */
    } else if (!strncmp(kind, "check", 5)) {
        struct check c;
        memset(&c, 0, sizeof(c));
        uint16_t refs[256] = { 0 };
        uint32_t queue[256];
        c.refs = refs; c.queue = queue;
        if (!ok) fail_read = (int)(2 + FILE_INUM / IPB);
        struct wdirent e = { .inum = FILE_INUM };
        strcpy(e.name, "file");
        rc = check_entry(&e, 0, &c);
        if (ok) CHECK(rc == 0 && e.inum == FILE_INUM && refs[FILE_INUM] == 1 && c.fixes == 0);
        else CHECK(e.inum == FILE_INUM && c.fixes == 0 && rc == 1);   /* 舊的程式:清掉這筆、回傳 3 */
    } else {
        return 91;
    }
    printf("{\"case\":\"%s\",\"rc\":%ld,\"errors\":%d}\n", kind, rc, errors);
    bcache_free();
    return errors != 0;
}
