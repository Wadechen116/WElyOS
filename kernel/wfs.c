/*
 * wfs.c - WFS:存在磁碟上的檔案系統(實作 fs.h 的 inode_ops)
 *
 * 和 ramfs 的差別只有一個:資料放在磁碟上,關機後還在。VFS 以上的程式完全不必改。
 *
 * ---- 磁碟上的配置 ----
 *
 * 磁碟被切成 4KB 的「區塊」(block,等於 8 個磁區),依序是:
 *
 *   區塊 0          superblock:描述整個檔案系統(大小、各區從哪裡開始、是否正常卸載)
 *   區塊位元圖      每個區塊一個位元:1 = 使用中
 *   inode 表        每個 inode 64 位元組(一個區塊放 64 個);編號 0 不使用,1 是根目錄
 *   資料區          檔案的內容、目錄的內容、間接區塊
 *
 * inode 記錄一個檔案或目錄:類型、大小、有幾個名稱指向它,以及「內容放在哪些區塊」:
 *   addr[0..9]   前 10 個區塊的編號(直接)
 *   indirect     一個「間接區塊」的編號,它裡面是 1024 個區塊編號(檔案的第 11 個區塊起)
 * 所以單一檔案最大 (10 + 1024) × 4KB,約 4MB。編號 0 表示「沒有配置」(讀起來是 0)。
 *
 * 目錄的內容是一連串固定 64 位元組的紀錄:{ inode 編號, 名稱 };編號 0 表示這一格是空的。
 * 目錄不儲存 "." 與 "..":上層目錄的編號記在 inode 裡(parent)。
 *
 * 所有多位元組的數值以 little-endian 儲存(三個支援的架構都是)。
 *
 * ---- 區塊快取 ----
 *
 * 每次讀寫都去碰磁碟太慢,所以記憶體裡留著最近用過的 64 個區塊(buffer cache)。
 * 修改只先改快取裡的那一份(標記為「髒」,dirty),之後才一次寫回磁碟(見 bsync):
 *   - 檔案關閉、建立、刪除之後
 *   - sync 系統呼叫、關機
 *   - 快取滿了,要把某個髒的區塊讓出來的時候
 *
 * ---- 當機之後 ----
 *
 * 一個操作通常要改好幾個區塊(位元圖、inode、目錄)。如果寫到一半就斷電,
 * 磁碟上的各部分會互相對不上。對策分兩層:
 *   1. 寫回的順序:先寫資料區、最後才寫位元圖與 superblock(區塊編號由大到小)。
 *      這樣中斷時最可能的結果是「多佔了沒有人用的區塊」,而不是「inode 指向別人的資料」
 *   2. superblock 裡有一個「正常卸載」的記號:掛載時清掉、關機時設回來。
 *      掛載時發現記號是清掉的(上次沒有正常關機),就做一次完整的檢查與修復
 *      (wfs_check):從根目錄走過所有目錄,重新算出哪些 inode 與區塊真的有人用
 *
 * ---- 不信任磁碟上的內容 ----
 *
 * 磁碟的內容可能損壞(或被人刻意改過)。所有從磁碟讀來的編號在使用之前都檢查範圍,
 * 不合理的一律當成錯誤(-E_IO)或「沒有配置」,不會讓 kernel 讀寫到別的地方。
 *
 * ---- 上鎖 ----
 *
 * 一把大鎖(wfs_lk)保護這裡所有的資料:快取、位元圖、inode 表。每個操作從頭鎖到尾。
 * 簡單、不會出錯;代價是同時只有一個操作在進行(磁碟本來也是一次做一件事)。
 * 鎖的順序:VFS 的 inode 鎖 -> wfs_lk。
 *
 * 記憶體全部在掛載時配置好,之後的操作不再配置,所以不會因為記憶體不足而做到一半失敗。
 */
#include <stddef.h>
#include "blk.h"
#include "fs.h"
#include "kprintf.h"
#include "kstring.h"
#include "pmm.h"
#include "sync.h"

#define WFS_MAGIC   0x31534657u              /* "WFS1" */
#define BSIZE       PAGE_SIZE
#define BSECT       (BSIZE / BLK_SECTOR)     /* 一個區塊是幾個磁區 */
#define NDIRECT     10u
#define NINDIRECT   (BSIZE / 4u)
#define MAX_FBLOCKS (NDIRECT + NINDIRECT)
#define ROOT_INUM   1u
#define MAX_INODES  4096u

struct wsuper {
    uint32_t magic;
    uint32_t nblocks;                        /* 整個檔案系統有幾個區塊 */
    uint32_t ninodes;
    uint32_t bitmap_start, bitmap_blocks;
    uint32_t inode_start, inode_blocks;
    uint32_t data_start;
    uint32_t clean;                          /* 1 = 上次正常卸載 */
    uint32_t mounts;                         /* 被掛載過幾次 */
};

struct dinode {                              /* 磁碟上的 inode,64 位元組 */
    uint16_t type;                           /* 0 = 這一格沒有使用;I_FILE;I_DIR */
    uint16_t nlink;
    uint32_t parent;                         /* 所在目錄的 inode 編號 */
    uint64_t size;
    uint32_t addr[NDIRECT];
    uint32_t indirect;
    uint32_t pad;
};
#define IPB (BSIZE / sizeof(struct dinode))

struct wdirent {                             /* 目錄裡的一筆紀錄,64 位元組 */
    uint32_t inum;                           /* 0 = 空的 */
    char name[FS_NAME_MAX + 1];
};
#define DPB (BSIZE / sizeof(struct wdirent))

/* 記憶體中的 inode:每個磁碟上的 inode 一格(以編號為索引),第一次用到時才從磁碟讀入 */
struct wnode {
    struct inode in;                         /* 必須是第一個成員 */
    uint32_t inum;
    int loaded;
    struct dinode d;
};

/* ---- 狀態(全部由 wfs_lk 保護) ---- */
static struct mutex wfs_lk;
static struct blkdev *disk;
static struct wsuper sb;
static struct wnode *nodes;                  /* ninodes 格 */
static uint32_t free_blocks, free_inodes;
static uint32_t balloc_hint, ialloc_hint;
static unsigned last_repairs;
static int mounted;
static const struct inode_ops wfs_ops;

static struct wnode *wn(struct inode *in) { return (struct wnode *)in; }

/* ------------------------------------------------------------------ */
/* 區塊快取                                                            */
/* ------------------------------------------------------------------ */

#define NBUF 64u

struct buf {
    uint32_t no;
    int valid, dirty, pins;                  /* pins:正在使用中(不可以被換掉)的次數 */
    unsigned long used;                      /* 最後一次使用的時間(越大越新) */
    uint8_t *data;
};
static struct buf bufs[NBUF];
static unsigned long use_clock;
static unsigned long disk_reads, disk_writes;

static int bwrite(struct buf *b)
{
    if (disk->rw(disk, (uint64_t)b->no * BSECT, BSECT, b->data, 1) != 0)
        return -E_IO;
    disk_writes++;
    b->dirty = 0;
    return 0;
}

/*
 * 取得第 no 個區塊在快取裡的那一份,並標記為使用中(用完要 bput)。
 *   load = 1:快取裡沒有的話從磁碟讀進來
 *   load = 0:呼叫者會把整個區塊覆蓋掉,不必讀;內容先清為 0
 * 失敗(讀取錯誤、編號超出範圍、全部的格子都在使用中)回傳 NULL。
 */
static struct buf *bget(uint32_t no, int load)
{
    struct buf *victim = NULL;

    if (no >= sb.nblocks)
        return NULL;
    for (struct buf *b = bufs; b < bufs + NBUF; b++) {
        if (b->valid && b->no == no) {
            b->pins++;
            b->used = ++use_clock;
            if (!load)
                memset(b->data, 0, BSIZE);
            return b;
        }
        /* 順便找出可以讓出來的一格:沒有內容的最好,否則是最久沒用的 */
        if (b->pins == 0 && (!victim || !b->valid || (victim->valid && b->used < victim->used)))
            victim = b;
    }
    if (!victim)
        return NULL;
    if (victim->valid && victim->dirty && bwrite(victim) != 0)
        return NULL;
    victim->valid = 0;
    if (load) {
        if (disk->rw(disk, (uint64_t)no * BSECT, BSECT, victim->data, 0) != 0)
            return NULL;
        disk_reads++;
    } else {
        memset(victim->data, 0, BSIZE);
    }
    victim->no = no;
    victim->valid = 1;
    victim->dirty = 0;
    victim->pins = 1;
    victim->used = ++use_clock;
    return victim;
}

static void bput(struct buf *b)
{
    b->pins--;
}

/*
 * 把所有髒的區塊寫回磁碟:編號由大到小(資料先寫,位元圖與 superblock 最後;理由見檔頭)。
 *
 * barrier:寫完之後,再要求磁碟把「它自己暫存的資料」真正寫到儲存媒體上(flush)。
 * 這一步很慢(在模擬器裡是對主機的檔案做一次同步,實測每次數十毫秒),
 * 所以只在使用者明確要求(sync)與關機時做;平常每個操作結束時的寫回不做。
 * 差別只在「整台主機突然斷電」時最後幾個操作可能還沒落到媒體上 —
 * 檔案系統本身仍然靠寫回的順序與掛載時的檢查保持一致。
 */
static int bsync_opt(int barrier)
{
    int rc = 0;

    for (;;) {
        struct buf *next = NULL;
        for (struct buf *b = bufs; b < bufs + NBUF; b++)
            if (b->valid && b->dirty && (!next || b->no > next->no))
                next = b;
        if (!next)
            break;
        if (bwrite(next) != 0) {
            next->dirty = 0;                 /* 寫不進去:放棄這一塊,免得永遠繞不出去 */
            rc = -E_IO;
        }
    }
    if (barrier && disk->flush && disk->flush(disk) != 0)
        rc = -E_IO;
    return rc;
}

static int bsync(void)
{
    return bsync_opt(0);
}

/* ------------------------------------------------------------------ */
/* 區塊與 inode 的配置                                                  */
/* ------------------------------------------------------------------ */

static int sb_write(void)
{
    struct buf *b = bget(0, 0);

    if (!b)
        return -E_IO;
    memcpy(b->data, &sb, sizeof(sb));
    b->dirty = 1;
    bput(b);
    return 0;
}

/* 讀取 / 設定位元圖中第 no 個區塊的位元。回傳原本的值;失敗回傳 -1 */
static int bitmap_op(uint32_t no, int set, int value)
{
    struct buf *b = bget(sb.bitmap_start + no / (BSIZE * 8), 1);

    if (!b)
        return -1;
    uint8_t *byte = &b->data[(no % (BSIZE * 8)) / 8];
    uint8_t mask = (uint8_t)(1u << (no % 8));
    int old = (*byte & mask) != 0;
    if (set && old != value) {
        *byte = value ? (*byte | mask) : (*byte & (uint8_t)~mask);
        b->dirty = 1;
    }
    bput(b);
    return old;
}

/* 配置一個資料區塊,回傳編號;沒有空間(或出錯)回傳 0 */
static uint32_t balloc(void)
{
    uint32_t span = sb.nblocks - sb.data_start;

    if (free_blocks == 0)
        return 0;
    for (uint32_t i = 0; i < span; i++) {
        uint32_t no = sb.data_start + (balloc_hint + i) % span;
        int used = bitmap_op(no, 0, 0);
        if (used < 0)
            return 0;
        if (!used) {
            bitmap_op(no, 1, 1);
            balloc_hint = (balloc_hint + i + 1) % span;
            free_blocks--;
            return no;
        }
    }
    return 0;
}

static void bfree(uint32_t no)
{
    if (no < sb.data_start || no >= sb.nblocks)
        return;                              /* 磁碟上的編號不合理:不理它 */
    if (bitmap_op(no, 1, 0) == 1)
        free_blocks++;
    /* 快取裡若還有這個區塊的髒內容,不必寫回了 */
    for (struct buf *b = bufs; b < bufs + NBUF; b++)
        if (b->valid && b->no == no && b->pins == 0)
            b->valid = b->dirty = 0;
}

static int dinode_rw(uint32_t inum, struct dinode *d, int write)
{
    struct buf *b = bget(sb.inode_start + inum / IPB, 1);

    if (!b)
        return -E_IO;
    struct dinode *slot = (struct dinode *)b->data + inum % IPB;
    if (write) {
        *slot = *d;
        b->dirty = 1;
    } else {
        *d = *slot;
    }
    bput(b);
    return 0;
}

static int iupdate(struct wnode *n)
{
    n->d.size = n->in.size;
    return dinode_rw(n->inum, &n->d, 1);
}

/* 取得編號 inum 的 inode(第一次用到時從磁碟讀入)。編號不合理或該格沒有使用回傳 NULL */
static struct wnode *nget(uint32_t inum)
{
    if (inum == 0 || inum >= sb.ninodes)
        return NULL;
    struct wnode *n = &nodes[inum];
    if (!n->loaded) {
        if (dinode_rw(inum, &n->d, 0) != 0)
            return NULL;
        if (n->d.type != I_FILE && n->d.type != I_DIR)
            return NULL;
        if (n->d.size > (uint64_t)MAX_FBLOCKS * BSIZE)
            n->d.size = (uint64_t)MAX_FBLOCKS * BSIZE;       /* 磁碟上的大小不合理:截到上限 */
        inode_init(&n->in, &wfs_ops, n->d.type);
        n->in.size = n->d.size;
        n->in.nlink = n->d.nlink;
        n->inum = inum;
        n->loaded = 1;
    }
    return n;
}

/* 配置一個新的 inode;沒有空位回傳 NULL */
static struct wnode *ialloc(int type, uint32_t parent)
{
    struct dinode d;

    for (uint32_t i = 0; i < sb.ninodes; i++) {
        uint32_t inum = (ialloc_hint + i) % sb.ninodes;
        if (inum == 0 || nodes[inum].loaded)
            continue;
        if (dinode_rw(inum, &d, 0) != 0)
            return NULL;
        if (d.type != 0)
            continue;
        memset(&d, 0, sizeof(d));
        d.type = (uint16_t)type;
        d.nlink = 1;
        d.parent = parent;
        if (dinode_rw(inum, &d, 1) != 0)
            return NULL;
        ialloc_hint = inum + 1;
        free_inodes--;
        return nget(inum);
    }
    return NULL;
}

/*
 * 檔案的第 fb 個區塊在磁碟上的編號。alloc 時,還沒有配置就配置一個(內容為 0)。
 * 回傳 0 = 沒有配置(alloc 時表示沒有空間了)。磁碟上不合理的編號也當成 0。
 */
static uint32_t bmap(struct wnode *n, uint32_t fb, int alloc)
{
    uint32_t no;

    if (fb < NDIRECT) {
        no = n->d.addr[fb];
        if (no == 0 && alloc && (no = balloc()) != 0) {
            struct buf *z = bget(no, 0);         /* 新的區塊一定清 0:不可以讓人讀到磁碟上舊的內容 */
            if (!z) {
                bfree(no);
                return 0;
            }
            z->dirty = 1;
            bput(z);
            n->d.addr[fb] = no;
        }
    } else if (fb < MAX_FBLOCKS) {
        if (n->d.indirect == 0) {
            if (!alloc || (no = balloc()) == 0)
                return 0;
            struct buf *z = bget(no, 0);
            if (!z) {
                bfree(no);
                return 0;
            }
            z->dirty = 1;
            bput(z);
            n->d.indirect = no;
        }
        struct buf *ib = bget(n->d.indirect, 1);
        if (!ib || n->d.indirect < sb.data_start)
            return ib ? (bput(ib), 0u) : 0u;
        uint32_t *tab = (uint32_t *)ib->data;
        no = tab[fb - NDIRECT];
        if (no == 0 && alloc && (no = balloc()) != 0) {
            struct buf *z = bget(no, 0);
            if (!z) {
                bfree(no);
                no = 0;
            } else {
                z->dirty = 1;
                bput(z);
                tab[fb - NDIRECT] = no;
                ib->dirty = 1;
            }
        }
        bput(ib);
    } else {
        return 0;
    }
    return no >= sb.data_start && no < sb.nblocks ? no : 0;
}

/* ------------------------------------------------------------------ */
/* 檔案內容(呼叫時已持有 wfs_lk)                                      */
/* ------------------------------------------------------------------ */

static long file_read(struct wnode *n, void *buf, size_t len, uint64_t off)
{
    uint8_t *out = buf;
    size_t done = 0;

    if (off >= n->in.size)
        return 0;
    if (len > n->in.size - off)
        len = (size_t)(n->in.size - off);
    while (done < len) {
        uint32_t fb = (uint32_t)((off + done) / BSIZE);
        size_t in_blk = (size_t)((off + done) % BSIZE);
        size_t k = BSIZE - in_blk;
        if (k > len - done)
            k = len - done;
        uint32_t no = bmap(n, fb, 0);
        if (no == 0) {
            memset(out + done, 0, k);            /* 沒有配置的部分讀起來是 0 */
        } else {
            struct buf *b = bget(no, 1);
            if (!b)
                return done ? (long)done : -E_IO;
            memcpy(out + done, b->data + in_blk, k);
            bput(b);
        }
        done += k;
    }
    return (long)done;
}

static long file_write(struct wnode *n, const void *buf, size_t len, uint64_t off)
{
    const uint8_t *src = buf;
    size_t done = 0;
    long err = -E_NOSPC;

    if (off + len > (uint64_t)MAX_FBLOCKS * BSIZE || off + len < off)
        return -E_FBIG;
    while (done < len) {
        uint32_t fb = (uint32_t)((off + done) / BSIZE);
        size_t in_blk = (size_t)((off + done) % BSIZE);
        size_t k = BSIZE - in_blk;
        if (k > len - done)
            k = len - done;
        uint32_t no = bmap(n, fb, 1);
        if (no == 0)
            break;                               /* 磁碟滿了:回報已經寫入的部分 */
        struct buf *b = bget(no, 1);
        if (!b) {
            err = -E_IO;
            break;
        }
        memcpy(b->data + in_blk, src + done, k);
        b->dirty = 1;
        bput(b);
        done += k;
    }
    if (off + done > n->in.size)
        n->in.size = off + done;
    if (iupdate(n) != 0)
        return -E_IO;
    return done || len == 0 ? (long)done : err;
}

/* 把檔案縮短到 size:釋放之後的區塊 */
static int file_truncate(struct wnode *n, uint64_t size)
{
    uint32_t keep = (uint32_t)((size + BSIZE - 1) / BSIZE);

    for (uint32_t fb = keep; fb < NDIRECT; fb++) {
        if (n->d.addr[fb]) {
            bfree(n->d.addr[fb]);
            n->d.addr[fb] = 0;
        }
    }
    if (n->d.indirect) {
        struct buf *ib = bget(n->d.indirect, 1);
        if (ib) {
            uint32_t *tab = (uint32_t *)ib->data;
            for (uint32_t i = keep > NDIRECT ? keep - NDIRECT : 0; i < NINDIRECT; i++) {
                if (tab[i]) {
                    bfree(tab[i]);
                    tab[i] = 0;
                    ib->dirty = 1;
                }
            }
            bput(ib);
        }
        if (keep <= NDIRECT) {
            bfree(n->d.indirect);
            n->d.indirect = 0;
        }
    }
    /* 留下的最後一個區塊中,超出新大小的部分清 0:之後檔案再變大時,那裡應該讀到 0 */
    if (size % BSIZE) {
        uint32_t no = bmap(n, keep - 1, 0);
        struct buf *b = no ? bget(no, 1) : NULL;
        if (b) {
            memset(b->data + size % BSIZE, 0, BSIZE - (size_t)(size % BSIZE));
            b->dirty = 1;
            bput(b);
        }
    }
    n->in.size = size;
    return iupdate(n);
}

/* ------------------------------------------------------------------ */
/* 目錄(呼叫時已持有 wfs_lk)                                          */
/* ------------------------------------------------------------------ */

/*
 * 走過目錄的每一格紀錄。對每一格呼叫 fn(e, slot, arg):
 *   回傳 0 繼續;1 = 找到了,停止(dir_scan 回傳 1);2 = 這一格被修改了,標記為髒並停止(回傳 1);
 *   3 = 這一格被修改了,標記為髒,繼續往下走
 * 走完都沒有停止回傳 0;讀取錯誤回傳 -E_IO。
 */
typedef int (*dir_fn)(struct wdirent *e, uint64_t slot, void *arg);

static int dir_scan(struct wnode *dir, uint64_t from_slot, dir_fn fn, void *arg)
{
    uint64_t nslots = dir->in.size / sizeof(struct wdirent);

    for (uint64_t slot = from_slot; slot < nslots; ) {
        uint32_t no = bmap(dir, (uint32_t)(slot / DPB), 0);
        if (no == 0) {
            slot = (slot / DPB + 1) * DPB;       /* 這個區塊沒有配置:整塊跳過 */
            continue;
        }
        struct buf *b = bget(no, 1);
        if (!b)
            return -E_IO;
        struct wdirent *ents = (struct wdirent *)b->data;
        for (; slot < nslots; slot++) {
            struct wdirent *e = &ents[slot % DPB];
            e->name[FS_NAME_MAX] = '\0';         /* 磁碟上的名稱不一定有結尾:自己補上 */
            int r = fn(e, slot, arg);
            if (r == 3) {
                b->dirty = 1;
            } else if (r) {
                if (r == 2)
                    b->dirty = 1;
                bput(b);
                return 1;
            }
            if ((slot + 1) % DPB == 0) {
                slot++;
                break;
            }
        }
        bput(b);
    }
    return 0;
}

struct find_arg {
    const char *name;
    uint32_t inum;
    int clear;                                   /* 找到之後把這一格清掉(刪除名稱) */
};

static int find_fn(struct wdirent *e, uint64_t slot, void *arg)
{
    struct find_arg *fa = arg;

    (void)slot;
    if (e->inum == 0 || strcmp(e->name, fa->name))
        return 0;
    fa->inum = e->inum;
    if (fa->clear) {
        memset(e, 0, sizeof(*e));
        return 2;
    }
    return 1;
}

static int used_fn(struct wdirent *e, uint64_t slot, void *arg)
{
    (void)slot; (void)arg;
    return e->inum != 0;
}

struct add_arg {
    const char *name;
    uint32_t inum;
};

static int add_fn(struct wdirent *e, uint64_t slot, void *arg)
{
    struct add_arg *aa = arg;

    (void)slot;
    if (e->inum != 0)
        return 0;
    memset(e, 0, sizeof(*e));
    e->inum = aa->inum;
    for (size_t i = 0; aa->name[i] && i < FS_NAME_MAX; i++)
        e->name[i] = aa->name[i];
    return 2;
}

/* 在目錄裡加一筆紀錄:先找空的格子,沒有就接在最後 */
static int dir_add(struct wnode *dir, const char *name, uint32_t inum)
{
    struct add_arg aa = { name, inum };
    struct wdirent e;
    int r = dir_scan(dir, 0, add_fn, &aa);

    if (r)
        return r < 0 ? r : 0;
    memset(&e, 0, sizeof(e));
    e.inum = inum;
    for (size_t i = 0; name[i] && i < FS_NAME_MAX; i++)
        e.name[i] = name[i];
    long w = file_write(dir, &e, sizeof(e), dir->in.size);
    if (w == (long)sizeof(e))
        return 0;
    return w < 0 ? (int)w : -E_NOSPC;
}

/* ------------------------------------------------------------------ */
/* inode_ops(VFS 呼叫的進入點;各自取得 wfs_lk)                        */
/* ------------------------------------------------------------------ */

static int wfs_lookup(struct inode *dir, const char *name, struct inode **out)
{
    struct wnode *d = wn(dir), *n;
    struct find_arg fa = { name, 0, 0 };
    int err = 0;

    mutex_lock(&wfs_lk);
    if (name[0] == '.' && name[1] == '.' && name[2] == '\0') {
        fa.inum = d->d.parent;                   /* 根目錄的上層是它自己(VFS 會處理掛載點) */
    } else {
        int r = dir_scan(d, 0, find_fn, &fa);
        if (r <= 0)
            err = r < 0 ? r : -E_NOENT;
    }
    if (err == 0) {
        n = nget(fa.inum);
        if (n)
            *out = &n->in;
        else
            err = -E_IO;                         /* 目錄指向一個不存在的 inode:磁碟內容損壞 */
    }
    mutex_unlock(&wfs_lk);
    return err;
}

static int wfs_create(struct inode *dir, const char *name, int type, struct inode **out)
{
    struct wnode *d = wn(dir), *n;
    int err;

    if (type != I_FILE && type != I_DIR)
        return -E_INVAL;                         /* 裝置節點只存在於記憶體檔案系統 */
    mutex_lock(&wfs_lk);
    n = ialloc(type, d->inum);
    if (!n) {
        err = -E_NOSPC;
    } else if ((err = dir_add(d, name, n->inum)) != 0) {
        n->d.type = 0;                           /* 目錄放不下:把剛配置的 inode 還回去 */
        dinode_rw(n->inum, &n->d, 1);
        n->loaded = 0;
        free_inodes++;
    } else {
        *out = &n->in;
    }
    if (bsync() != 0 && err == 0)
        err = -E_IO;
    mutex_unlock(&wfs_lk);
    return err;
}

static int wfs_unlink(struct inode *dir, const char *name, struct inode **victim)
{
    struct find_arg fa = { name, 0, 1 };
    int err = 0;

    mutex_lock(&wfs_lk);
    int r = dir_scan(wn(dir), 0, find_fn, &fa);
    if (r <= 0) {
        err = r < 0 ? r : -E_NOENT;
    } else {
        struct wnode *n = nget(fa.inum);
        if (!n) {
            err = -E_IO;
        } else {
            /* 磁碟上的名稱數跟著減一。降到 0 而且沒有人開著時,VFS 接著會呼叫 release
               把它釋放;還有人開著的話先留著(這時當機,下次掛載的檢查會把它收掉) */
            if (n->d.nlink > 0)
                n->d.nlink--;
            iupdate(n);
            *victim = &n->in;
        }
    }
    if (bsync() != 0 && err == 0)
        err = -E_IO;
    mutex_unlock(&wfs_lk);
    return err;
}

struct list_arg {
    struct dirent *out;
    uint64_t next;
};

static int list_fn(struct wdirent *e, uint64_t slot, void *arg)
{
    struct list_arg *la = arg;

    if (e->inum == 0)
        return 0;
    struct wnode *n = nget(e->inum);
    memset(la->out, 0, sizeof(*la->out));
    memcpy(la->out->name, e->name, FS_NAME_MAX);
    la->out->type = n ? (uint32_t)n->d.type : 0;
    la->next = slot + 1;
    return 1;
}

static int wfs_readdir(struct inode *dir, uint64_t *pos, struct dirent *out)
{
    struct list_arg la = { out, 0 };

    mutex_lock(&wfs_lk);
    int r = dir_scan(wn(dir), *pos, list_fn, &la);
    mutex_unlock(&wfs_lk);
    if (r < 0)
        return r;
    if (r == 0)
        return 1;                                /* 沒有更多了 */
    *pos = la.next;
    return 0;
}

static int wfs_is_empty(struct inode *dir)
{
    mutex_lock(&wfs_lk);
    int r = dir_scan(wn(dir), 0, used_fn, NULL);
    mutex_unlock(&wfs_lk);
    return r == 0;
}

static long wfs_read(struct inode *in, void *buf, size_t n, uint64_t off)
{
    mutex_lock(&wfs_lk);
    long r = file_read(wn(in), buf, n, off);
    mutex_unlock(&wfs_lk);
    return r;
}

static long wfs_write(struct inode *in, const void *buf, size_t n, uint64_t off)
{
    mutex_lock(&wfs_lk);
    long r = file_write(wn(in), buf, n, off);
    mutex_unlock(&wfs_lk);
    return r;
}

static int wfs_truncate(struct inode *in, uint64_t size)
{
    mutex_lock(&wfs_lk);
    int r = size <= in->size ? file_truncate(wn(in), size) : -E_INVAL;
    if (bsync() != 0 && r == 0)
        r = -E_IO;
    mutex_unlock(&wfs_lk);
    return r;
}

/* 一次開啟被關閉了。寫入過的檔案在這時寫回磁碟:程式關閉檔案之後,資料就在磁碟上了 */
static void wfs_closed(struct inode *in, int flags)
{
    (void)in;
    if ((flags & O_ACCMODE) == O_RDONLY)
        return;
    mutex_lock(&wfs_lk);
    bsync();
    mutex_unlock(&wfs_lk);
}

/* 沒有任何名稱、也沒有人開著了:釋放它的區塊與 inode */
static void wfs_release(struct inode *in)
{
    struct wnode *n = wn(in);

    mutex_lock(&wfs_lk);
    file_truncate(n, 0);
    n->d.type = 0;
    dinode_rw(n->inum, &n->d, 1);
    n->loaded = 0;
    free_inodes++;
    if (n->inum < ialloc_hint)
        ialloc_hint = n->inum;
    bsync();
    mutex_unlock(&wfs_lk);
}

static int wfs_statfs(struct inode *in, struct fsinfo *out)
{
    (void)in;
    mutex_lock(&wfs_lk);
    memset(out, 0, sizeof(*out));
    out->block_size = BSIZE;
    out->blocks = sb.nblocks - sb.data_start;
    out->free_blocks = free_blocks;
    out->inodes = sb.ninodes - 1;
    out->free_inodes = free_inodes;
    memcpy(out->type, "wfs", 4);
    mutex_unlock(&wfs_lk);
    return 0;
}

static int wfs_sync_op(struct inode *in)
{
    (void)in;
    mutex_lock(&wfs_lk);
    int r = bsync_opt(1);
    mutex_unlock(&wfs_lk);
    return r;
}

static const struct inode_ops wfs_ops = {
    .lookup = wfs_lookup,
    .create = wfs_create,
    .unlink = wfs_unlink,
    .readdir = wfs_readdir,
    .is_empty = wfs_is_empty,
    .read = wfs_read,
    .write = wfs_write,
    .truncate = wfs_truncate,
    .closed = wfs_closed,
    .release = wfs_release,
    .statfs = wfs_statfs,
    .sync = wfs_sync_op,
};

/* ------------------------------------------------------------------ */
/* 檢查與修復                                                          */
/* ------------------------------------------------------------------ */

struct check {
    uint8_t *bitmap;                             /* 重新算出來的位元圖 */
    uint16_t *refs;                              /* 每個 inode 被幾個目錄紀錄指到 */
    uint32_t *queue;                             /* 還沒走過的目錄 */
    uint32_t qhead, qtail;
    unsigned fixes;
};

static int claim(struct check *c, uint32_t no)
{
    if (no < sb.data_start || no >= sb.nblocks || (c->bitmap[no / 8] & (1u << (no % 8))))
        return 0;                                /* 超出範圍,或已經被別人佔用 */
    c->bitmap[no / 8] |= (uint8_t)(1u << (no % 8));
    return 1;
}

/* 目錄的一格紀錄:指向不存在的 inode 就清掉;否則記一次參考,第一次看到的目錄排進佇列 */
static int check_entry(struct wdirent *e, uint64_t slot, void *arg)
{
    struct check *c = arg;
    struct dinode d;

    (void)slot;
    if (e->inum == 0)
        return 0;
    int ok = e->inum < sb.ninodes && e->name[0] && dinode_rw(e->inum, &d, 0) == 0 &&
             (d.type == I_FILE || d.type == I_DIR) &&
             !(d.type == I_DIR && c->refs[e->inum] > 0);     /* 目錄只能有一個名稱 */
    if (!ok) {
        memset(e, 0, sizeof(*e));
        c->fixes++;
        return 3;                                /* 清掉了,繼續往下走 */
    }
    if (c->refs[e->inum]++ == 0 && d.type == I_DIR)
        c->queue[c->qtail++] = e->inum;
    return 0;
}

/*
 * 完整檢查:以「從根目錄走得到的東西」為準,修正其他不一致的地方。
 *   - 目錄裡指向不存在 inode 的紀錄:清掉
 *   - 走不到的 inode(沒有名稱、或上次刪到一半):釋放
 *   - 名稱數不對的 inode:改成實際的數目
 *   - 不合理、或和別的檔案重複的區塊編號:拿掉(那一段變成沒有配置)
 *   - 位元圖:依實際使用的區塊重新產生
 * 回傳修正的數目;無法進行(根目錄壞了、記憶體不足、讀寫錯誤)回傳負值。
 * 呼叫時持有 wfs_lk。
 *
 * at_mount = 1:掛載時、還沒有任何人使用這個檔案系統。記憶體裡的 inode 全部作廢,
 *               之後用到時重新從(修正過的)磁碟讀入
 * at_mount = 0:使用中再檢查一次(測試用,預期找不到任何問題):不可以作廢記憶體裡的 inode —
 *               VFS 還拿著它們的指標與參考計數
 */
static int wfs_check(int at_mount)
{
    struct check c = { 0 };
    size_t bm_pages = sb.bitmap_blocks, ref_pages = (sb.ninodes * 2 + BSIZE - 1) / BSIZE;
    size_t q_pages = (sb.ninodes * 4 + BSIZE - 1) / BSIZE;
    struct dinode d;
    int rc = -E_NOMEM;

    c.bitmap = pmm_alloc_pages(bm_pages);
    c.refs = pmm_alloc_pages(ref_pages);
    c.queue = pmm_alloc_pages(q_pages);
    if (!c.bitmap || !c.refs || !c.queue)
        goto out;
    memset(c.bitmap, 0, bm_pages * BSIZE);
    memset(c.refs, 0, ref_pages * BSIZE);
    for (uint32_t no = 0; no < sb.data_start; no++)
        c.bitmap[no / 8] |= (uint8_t)(1u << (no % 8));

    for (uint32_t i = 0; at_mount && i < sb.ninodes; i++)
        nodes[i].loaded = 0;

    rc = -E_IO;
    struct wnode *root = nget(ROOT_INUM);
    if (!root || root->d.type != I_DIR)
        goto out;
    c.refs[ROOT_INUM] = 1;
    c.queue[c.qtail++] = ROOT_INUM;
    while (c.qhead < c.qtail) {
        struct wnode *dir = nget(c.queue[c.qhead++]);
        if (!dir || dir_scan(dir, 0, check_entry, &c) < 0)
            goto out;
    }

    free_inodes = 0;
    for (uint32_t inum = 1; inum < sb.ninodes; inum++) {
        int changed = 0;

        if (dinode_rw(inum, &d, 0) != 0)
            goto out;
        if (d.type == 0) {
            free_inodes++;
            continue;
        }
        if (c.refs[inum] == 0) {                 /* 走不到:釋放(它的區塊不標記,等於一起釋放) */
            memset(&d, 0, sizeof(d));
            dinode_rw(inum, &d, 1);
            if (at_mount)
                nodes[inum].loaded = 0;
            free_inodes++;
            c.fixes++;
            continue;
        }
        if (d.nlink != c.refs[inum]) {
            d.nlink = c.refs[inum];
            changed = 1;
        }
        if (d.size > (uint64_t)MAX_FBLOCKS * BSIZE) {
            d.size = (uint64_t)MAX_FBLOCKS * BSIZE;
            changed = 1;
        }
        for (unsigned i = 0; i < NDIRECT; i++) {
            if (d.addr[i] && !claim(&c, d.addr[i])) {
                d.addr[i] = 0;
                changed = 1;
            }
        }
        if (d.indirect && !claim(&c, d.indirect)) {
            d.indirect = 0;
            changed = 1;
        }
        if (d.indirect) {
            struct buf *ib = bget(d.indirect, 1);
            if (!ib)
                goto out;
            uint32_t *tab = (uint32_t *)ib->data;
            for (unsigned i = 0; i < NINDIRECT; i++) {
                if (tab[i] && !claim(&c, tab[i])) {
                    tab[i] = 0;
                    ib->dirty = 1;
                    c.fixes++;
                }
            }
            bput(ib);
        }
        if (changed) {
            dinode_rw(inum, &d, 1);
            if (at_mount)
                nodes[inum].loaded = 0;          /* 下次用到時重新讀入修正後的內容 */
            c.fixes++;
        }
    }

    /* 位元圖:和重新算出來的不一樣就換掉 */
    free_blocks = 0;
    for (uint32_t no = sb.data_start; no < sb.nblocks; no++)
        free_blocks += !(c.bitmap[no / 8] & (1u << (no % 8)));
    for (uint32_t i = 0; i < sb.bitmap_blocks; i++) {
        struct buf *b = bget(sb.bitmap_start + i, 1);
        if (!b)
            goto out;
        if (memcmp(b->data, c.bitmap + (size_t)i * BSIZE, BSIZE)) {
            memcpy(b->data, c.bitmap + (size_t)i * BSIZE, BSIZE);
            b->dirty = 1;
            c.fixes++;
        }
        bput(b);
    }
    rc = bsync() == 0 ? (int)c.fixes : -E_IO;
out:
    if (c.bitmap)
        pmm_free_pages(c.bitmap, bm_pages);
    if (c.refs)
        pmm_free_pages(c.refs, ref_pages);
    if (c.queue)
        pmm_free_pages(c.queue, q_pages);
    return rc;
}

/* ------------------------------------------------------------------ */
/* 格式化與掛載                                                        */
/* ------------------------------------------------------------------ */

/* 依總區塊數與 inode 數算出各區的位置(格式化與驗證 superblock 都用同一套算法) */
static void layout(struct wsuper *s, uint32_t nblocks, uint32_t ninodes)
{
    memset(s, 0, sizeof(*s));
    s->magic = WFS_MAGIC;
    s->nblocks = nblocks;
    s->ninodes = ninodes;
    s->bitmap_start = 1;
    s->bitmap_blocks = (nblocks + BSIZE * 8 - 1) / (BSIZE * 8);
    s->inode_start = s->bitmap_start + s->bitmap_blocks;
    s->inode_blocks = (ninodes + IPB - 1) / IPB;
    s->data_start = s->inode_start + s->inode_blocks;
}

/* 在空白的磁碟上建立一個空的檔案系統 */
static int wfs_format(uint32_t nblocks)
{
    uint32_t ninodes = nblocks / 8;              /* 平均每 32KB 一個檔案 */
    struct dinode root;

    ninodes = ninodes < 64 ? 64 : ninodes > MAX_INODES ? MAX_INODES : ninodes;
    ninodes = (ninodes + IPB - 1) / IPB * IPB;
    layout(&sb, nblocks, ninodes);
    if (sb.data_start + 8 > nblocks)
        return -E_NOSPC;                         /* 磁碟太小 */

    for (uint32_t no = 1; no < sb.data_start; no++) {    /* 位元圖與 inode 表全部清 0 */
        struct buf *b = bget(no, 0);
        if (!b)
            return -E_IO;
        b->dirty = 1;
        bput(b);
        if (no % 32 == 0 && bsync() != 0)
            return -E_IO;
    }
    for (uint32_t no = 0; no < sb.data_start; no++)
        if (bitmap_op(no, 1, 1) < 0)
            return -E_IO;
    /* 位元圖最後一個區塊中,超出磁碟大小的位元也標記為使用中,配置時就不會選到 */
    for (uint32_t no = nblocks; no < sb.bitmap_blocks * BSIZE * 8; no++)
        if (bitmap_op(no, 1, 1) < 0)
            return -E_IO;
    memset(&root, 0, sizeof(root));
    root.type = I_DIR;
    root.nlink = 1;
    root.parent = ROOT_INUM;
    sb.clean = 1;
    if (dinode_rw(ROOT_INUM, &root, 1) != 0 || sb_write() != 0)
        return -E_IO;
    return bsync();
}

int wfs_mount(struct blkdev *dev, struct inode **root_out)
{
    struct wsuper want;
    int err = -E_IO, formatted = 0;
    uint64_t dev_blocks = dev->sectors / BSECT;

    mutex_init(&wfs_lk);
    if (mounted)
        return -E_BUSY;
    if (dev_blocks < 64)
        return -E_NOSPC;                         /* 磁碟太小(不到 256KB) */
    if (dev_blocks > 0x100000)
        dev_blocks = 0x100000;                   /* 最多使用前 4GB */

    /* 快取的緩衝區與 inode 表:現在一次配置好,之後的操作都不再配置記憶體 */
    size_t node_pages = (MAX_INODES * sizeof(struct wnode) + PAGE_SIZE - 1) / PAGE_SIZE;
    uint8_t *cache = pmm_alloc_pages(NBUF);
    nodes = pmm_alloc_pages(node_pages);
    if (!cache || !nodes) {
        err = -E_NOMEM;
        goto fail;
    }
    memset(nodes, 0, node_pages * PAGE_SIZE);
    memset(bufs, 0, sizeof(bufs));
    for (unsigned i = 0; i < NBUF; i++)
        bufs[i].data = cache + (size_t)i * BSIZE;

    mutex_lock(&wfs_lk);
    disk = dev;
    sb.nblocks = 1;                              /* 先只允許讀第 0 個區塊 */
    struct buf *b = bget(0, 1);
    if (!b)
        goto fail_locked;
    memcpy(&sb, b->data, sizeof(sb));
    int blank = 1;
    for (unsigned i = 0; i < BSIZE && blank; i++)
        blank = b->data[i] == 0;
    bput(b);
    b->valid = 0;

    if (blank) {
        /* 全新的磁碟(開頭全是 0):建立檔案系統。開頭有別的內容時絕不格式化 —
           那可能是別人的資料 */
        sb.nblocks = (uint32_t)dev_blocks;
        err = wfs_format((uint32_t)dev_blocks);
        if (err)
            goto fail_locked;
        formatted = 1;
    }
    /* superblock 來自磁碟,不可信任:各區的位置必須和依大小算出來的完全一致 */
    err = -E_INVAL;
    if (sb.magic != WFS_MAGIC || sb.nblocks < 64 || sb.nblocks > dev_blocks ||
        sb.ninodes < IPB || sb.ninodes > MAX_INODES)
        goto fail_locked;
    layout(&want, sb.nblocks, sb.ninodes);
    if (sb.bitmap_start != want.bitmap_start || sb.bitmap_blocks != want.bitmap_blocks ||
        sb.inode_start != want.inode_start || sb.inode_blocks != want.inode_blocks ||
        sb.data_start != want.data_start || sb.data_start + 8 > sb.nblocks)
        goto fail_locked;

    last_repairs = 0;
    if (!sb.clean) {
        /* 上次沒有正常關機:檢查並修復(同時算出空閒的數量) */
        int fixes = wfs_check(1);
        if (fixes < 0) {
            err = fixes;
            goto fail_locked;
        }
        last_repairs = (unsigned)fixes;
        kprintf("disk: the file system was not shut down cleanly; checked it and repaired "
                "%d problem(s)\n", fixes);
    } else {
        /* 算出空閒的區塊與 inode 數(之後由配置與釋放維護) */
        struct dinode d;
        free_blocks = free_inodes = 0;
        for (uint32_t i = 0; i < sb.bitmap_blocks; i++) {
            struct buf *bm = bget(sb.bitmap_start + i, 1);
            if (!bm)
                goto fail_locked;
            for (uint32_t bit = 0; bit < BSIZE * 8; bit++) {
                uint32_t no = i * BSIZE * 8 + bit;
                if (no >= sb.data_start && no < sb.nblocks)
                    free_blocks += !(bm->data[bit / 8] & (1u << (bit % 8)));
            }
            bput(bm);
        }
        for (uint32_t inum = 1; inum < sb.ninodes; inum++) {
            if (dinode_rw(inum, &d, 0) != 0)
                goto fail_locked;
            free_inodes += d.type == 0;
        }
    }
    struct wnode *root = nget(ROOT_INUM);
    if (!root || root->d.type != I_DIR)
        goto fail_locked;
    root->in.nlink = 1;                          /* 根目錄永遠存在 */

    /* 記下「使用中」:這次如果沒有正常關機,下次掛載時就會檢查 */
    sb.clean = 0;
    sb.mounts++;
    err = -E_IO;
    if (sb_write() != 0 || bsync() != 0)
        goto fail_locked;
    mounted = 1;
    mutex_unlock(&wfs_lk);
    *root_out = &root->in;
    return formatted ? 1 : 0;

fail_locked:
    mutex_unlock(&wfs_lk);
fail:
    if (cache)
        pmm_free_pages(cache, NBUF);
    if (nodes)
        pmm_free_pages(nodes, node_pages);
    nodes = NULL;
    disk = NULL;
    return err < 0 ? err : -E_IO;
}

/* 關機前呼叫:寫回所有資料,並記下「正常卸載」。之後不可以再使用這個檔案系統 */
void wfs_shutdown(void)
{
    if (!mounted)
        return;
    mutex_lock(&wfs_lk);
    bsync();
    sb.clean = 1;
    sb_write();
    bsync_opt(1);
    mounted = 0;
    mutex_unlock(&wfs_lk);
}

void wfs_stats(struct wfs_stats *st)
{
    memset(st, 0, sizeof(*st));
    if (!mounted)
        return;
    mutex_lock(&wfs_lk);
    st->mounted = 1;
    st->blocks = sb.nblocks;
    st->free_blocks = free_blocks;
    st->inodes = sb.ninodes - 1;
    st->free_inodes = free_inodes;
    st->mounts = sb.mounts;
    st->repairs = last_repairs;
    st->disk_reads = disk_reads;
    st->disk_writes = disk_writes;
    mutex_unlock(&wfs_lk);
}

int wfs_check_now(void)
{
    if (!mounted)
        return -E_INVAL;
    mutex_lock(&wfs_lk);
    int saved_free_b = (int)free_blocks, saved_free_i = (int)free_inodes;
    int r = bsync() == 0 ? wfs_check(0) : -E_IO;
    /* 檢查會重新讀入 inode,只能在沒有檔案開啟時使用(測試用) */
    if (r == 0 && ((int)free_blocks != saved_free_b || (int)free_inodes != saved_free_i))
        r = -E_IO;                               /* 記憶體裡的計數和磁碟上的不一致 */
    mutex_unlock(&wfs_lk);
    return r;
}

/*
 * 測試用:故意把磁碟弄成「當機到一半」的樣子,用來驗證掛載時的檢查與修復。
 *   - 佔用三個沒有人用的區塊
 *   - 配置一個沒有名稱的 inode(帶一個區塊)
 *   - 在根目錄放一筆指向不存在 inode 的紀錄(名稱 "ghost")
 *   - 把 victim_inum 那個檔案的名稱數改成錯的
 * 做完之後寫回磁碟,但不記「正常卸載」。
 */
int wfs_test_damage(const char *victim_name)
{
    struct find_arg fa = { victim_name, 0, 0 };
    int rc = -E_IO;

    if (!mounted)
        return -E_INVAL;
    mutex_lock(&wfs_lk);
    struct wnode *root = nget(ROOT_INUM);
    if (!root || dir_scan(root, 0, find_fn, &fa) != 1)
        goto out;
    for (int i = 0; i < 3; i++)
        if (!balloc())
            goto out;
    struct wnode *orphan = ialloc(I_FILE, ROOT_INUM);
    if (!orphan || file_write(orphan, "orphan", 6, 0) != 6)
        goto out;
    orphan->loaded = 0;
    if (dir_add(root, "ghost", sb.ninodes - 1) != 0)
        goto out;
    struct wnode *v = nget(fa.inum);
    if (!v)
        goto out;
    v->d.nlink = 5;
    iupdate(v);
    rc = bsync();
out:
    mutex_unlock(&wfs_lk);
    return rc;
}
