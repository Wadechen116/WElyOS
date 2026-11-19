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
 * ---- 檢查碼(WFS2) ----
 *
 * 儲存媒體上的資料會無聲無息地壞掉:某個位元翻轉了(在太空中是輻射,在地面上是老化的媒體),
 * 磁碟照樣回報「讀取成功」。沒有檢查碼的檔案系統會把壞掉的資料當成正確的交出去,
 * 或者照著壞掉的區塊編號去讀寫別的地方。
 *
 * 所以每個區塊有一個 CRC-32,存在另外一張表裡(位元圖之後、inode 表之前):
 *   寫入一個區塊   算出它的檢查碼,記進表裡
 *   從磁碟讀進來   重算一次,和表裡的不符就當作「讀取失敗」(-E_IO),絕不把它交給上層
 * 檢查碼和資料分開存放,是為了抓到「寫到別的地方去了」「這次寫入根本沒有發生」這類錯誤:
 * 如果檢查碼就放在區塊自己裡面,一個完整但過時(或放錯位置)的區塊看起來仍然是對的。
 * 表格整份留在記憶體裡,所以驗證不需要額外的磁碟讀取;只在 sync 與關機時寫回磁碟。
 *
 * superblock 不在表裡,它自己帶一個檢查碼,而且有兩份:一份在區塊 0,一份在檔案系統的
 * 最後一個區塊。掛載時第一份壞了就用第二份,並把第一份修好。
 *
 * 巡檢(scrub):很久沒被讀到的區塊就算壞了也沒有人知道。一條背景執行緒慢慢地把整顆磁碟
 * 讀過一遍又一遍,逐塊核對檢查碼,在資料還沒被用到之前就發現損壞。
 *
 * 能做到的是「發現」,不是「修復」:這裡沒有第二份資料可以拿來還原。
 * 當機之後檢查碼表可能比資料舊(資料寫了、表還沒寫),所以沒有正常關機的那一次掛載
 * 會依磁碟上實際的內容重新計算整張表 — 那一次等於沒有檢查碼的保護。
 *
 * ---- 寫入失敗之後 ----
 *
 * 寫回磁碟失敗時,記憶體裡的狀態(「已經建立了這個檔案」)和磁碟上的(「沒有」)就不一致了,
 * 而且不知道是哪些區塊沒寫成功。繼續在上面修改,只會把不一致擴大。
 * 所以一旦寫入失敗,整個檔案系統轉為「唯讀」:之後所有會修改的操作都回報 -E_IO,
 * 讀取照常(記憶體裡的快取與磁碟上的舊內容都還能用)。關機時不記「正常卸載」,
 * 下次掛載會做完整的檢查與修復。
 * 讀取失敗則不影響狀態:那一次操作回報 -E_IO,磁碟恢復之後可以再試。
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
#include "bcache.h"
#include "blk.h"
#include "crc32.h"
#include "fs.h"
#include "kprintf.h"
#include "kstring.h"
#include "panic.h"
#include "pmm.h"
#include "sync.h"
#include "timer.h"

#define WFS_MAGIC1  0x31534657u              /* "WFS1":第 27 步的格式,沒有檢查碼(仍然可以掛載) */
#define WFS_MAGIC   0x32534657u              /* "WFS2":第 38 步起的格式,見下方「檢查碼」 */
#define BSIZE       PAGE_SIZE                /* = BCACHE_BSIZE(bcache.c 有編譯期檢查) */
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
    /* ---- 以下只有 WFS2 有 ---- */
    uint32_t csum_start, csum_blocks;        /* 檢查碼表:每個區塊 4 個位元組 */
    uint32_t crc;                            /* 這個結構前面所有欄位的 CRC-32 */
};

/*
 * 磁碟上的 inode,64 位元組。
 * 第 34 步加入擁有者與權限時,沒有增加大小,而是把兩個「原本太寬」的欄位切開:
 *   parent  32 位元 -> 16(inode 最多 4096 個)+ mode 16
 *   size    64 位元 -> 32(單一檔案最大約 4MB)+ uid 16 + gid 16
 * 數值是 little-endian,原本的低位部分留在原位,所以舊的磁碟映像照常可讀:
 * 新欄位在舊資料裡都是 0,即「擁有者 root、權限未記錄」。mode 的最高位元(DMODE_VALID)
 * 用來分辨「權限是 000」與「舊資料,沒有記錄」,後者使用預設值。
 */
#define DMODE_VALID 0x8000u

struct dinode {
    uint16_t type;                           /* 0 = 這一格沒有使用;I_FILE;I_DIR */
    uint16_t nlink;
    uint16_t parent;                         /* 所在目錄的 inode 編號 */
    uint16_t mode;                           /* DMODE_VALID | 權限位元;0 = 沒有記錄 */
    uint32_t size;
    uint16_t uid, gid;                       /* 擁有者與群組(16 位元:編號上限 65535) */
    uint32_t addr[NDIRECT];
    uint32_t indirect;
    uint32_t mtime;                          /* 最後修改的時間(Unix 時間,秒;32 位元可以用到 2106 年) */
};
_Static_assert(sizeof(struct dinode) == 64, "the on-disk inode must stay 64 bytes");
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
static int csum_on;                          /* 這個檔案系統有檢查碼(WFS2) */
static int csum_verify;                      /* 讀取時核對檢查碼。沒有正常關機之後,表格重算完成之前是 0:
                                                那段期間表格可能比資料舊,拿它來核對只會誤判 */
static uint32_t *csum;                       /* 整張檢查碼表在記憶體裡的複本;0 = 沒有記錄 */
static uint8_t csum_dirty[1024];             /* 表的第 i 個區塊改過了、還沒寫回磁碟 */
static uint8_t *scratch;                     /* 一頁暫存空間(巡檢、重算檢查碼時直接讀磁碟用) */
static unsigned long csum_errors, scrub_errors, scrub_blocks;
static int sb_restored;                      /* 這次掛載時,superblock 是從備份救回來的 */
static int fs_failed;                        /* 寫入磁碟失敗過:之後拒絕所有修改(見下方「寫入失敗之後」) */
static const struct inode_ops wfs_ops;

static struct wnode *wn(struct inode *in) { return (struct wnode *)in; }

/* ------------------------------------------------------------------ */
/* 區塊快取                                                            */
/* ------------------------------------------------------------------ */

/* 快取本身在 kernel/bcache.c(bget / bput / bcache_sync);這裡是檔案系統接進去的部分:
   讀進來時核對檢查碼、寫回去後更新檢查碼、寫不進去就轉為唯讀 */

/* 這個區塊有沒有記在檢查碼表裡:superblock 的兩份(自己帶檢查碼)與表格本身除外 */
static int covered(uint32_t no)
{
    return csum_on && no != 0 && no != sb.nblocks - 1 &&
           !(no >= sb.csum_start && no < sb.csum_start + sb.csum_blocks);
}

static uint32_t block_crc(const void *data)
{
    uint32_t c = crc32(data, BSIZE);
    return c ? c : 1;                        /* 0 保留給「沒有記錄」 */
}

static void csum_set(uint32_t no, uint32_t value)
{
    if (csum[no] != value) {
        csum[no] = value;
        csum_dirty[no / (BSIZE / 4)] = 1;
    }
}

/* 把改過的檢查碼表寫回磁碟(在資料都寫完之後) */
static int csum_flush(void)
{
    int rc = 0;

    for (uint32_t i = 0; csum_on && i < sb.csum_blocks; i++) {
        if (!csum_dirty[i])
            continue;
        if (disk->rw(disk, (uint64_t)(sb.csum_start + i) * BSECT, BSECT,
                     (uint8_t *)csum + (size_t)i * BSIZE, 1) != 0) {
            fs_failed = 1;
            rc = -E_IO;
        } else {
            csum_dirty[i] = 0;
            bcache_count_write();
        }
    }
    return rc;
}

/* ---- 快取的三個回呼 ---- */

/* 剛從磁碟讀進來:磁碟說讀成功了,但內容和當初寫進去的一樣嗎?不一樣就當作讀取失敗 */
static int cache_verify(uint32_t no, const uint8_t *data)
{
    if (csum_verify && covered(no) && csum[no] && csum[no] != block_crc(data)) {
        if (csum_errors++ < 3)
            kprintf("disk: block %u is corrupted (checksum mismatch); the read is refused\n", no);
        return -1;
    }
    return 0;
}

/* 成功寫回磁碟:記下新內容的檢查碼 */
static void cache_written(uint32_t no, const uint8_t *data)
{
    if (covered(no))
        csum_set(no, block_crc(data));
}

/* 寫不進去:從此拒絕所有修改(見檔頭「寫入失敗之後」) */
static void cache_write_failed(uint32_t no)
{
    if (!fs_failed)
        kprintf("disk: write error on block %u; the file system is now read-only\n", no);
    fs_failed = 1;
}

static const struct bcache_ops cache_ops = { cache_verify, cache_written, cache_write_failed };

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
    int rc = bcache_sync();

    /* 檢查碼表只在 barrier(sync、關機)時寫回磁碟,不是每次同步都寫:
       掛載期間以記憶體裡的那一份為準;而沒有正常關機的話,下次掛載本來就會依磁碟的內容
       整張重算(見 csum_rebuild),中途寫回去的表用不到。省下的是每個操作一次以上的磁碟寫入 */
    if (barrier && csum_flush() != 0)
        rc = -E_IO;
    if (barrier && disk->flush && disk->flush(disk) != 0)
        rc = -E_IO;
    return fs_failed ? -E_IO : rc;
}

static int bsync(void)
{
    return bsync_opt(0);
}

/* ------------------------------------------------------------------ */
/* 區塊與 inode 的配置                                                  */
/* ------------------------------------------------------------------ */

/* superblock 的檢查碼(只算到 crc 欄位之前) */
static uint32_t sb_crc(const struct wsuper *s)
{
    return crc32(s, offsetof(struct wsuper, crc));
}

/* 把 superblock 放進快取等著寫回。WFS2 有兩份:區塊 0 與最後一個區塊。
   同步時依編號由大到小寫,所以備份先落地、主要的那一份最後;任何時候當機,至少有一份是完整的 */
static int sb_write(void)
{
    if (csum_on)
        sb.crc = sb_crc(&sb);
    for (int copy = 0; copy < (csum_on ? 2 : 1); copy++) {
        struct buf *b = bget(copy ? sb.nblocks - 1 : 0, 0);
        if (!b)
            return -E_IO;
        memcpy(b->data, &sb, sizeof(sb));
        b->dirty = 1;
        bput(b);
    }
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
    int was = bitmap_op(no, 1, 0);
    if (was == 1)
        free_blocks++;
    else if (was < 0)
        fs_failed = 1;                       /* 位元圖讀不到:這個區塊沒有真的被釋放,而呼叫者接著會忘掉它。
                                                轉為唯讀,讓下次掛載的檢查把它收回來 */
    if (covered(no))
        csum_set(no, 0);                     /* 不再有內容:之後的巡檢不必核對它 */
    bcache_forget(no);                       /* 快取裡若還有這個區塊的髒內容,不必寫回了 */
}

static int dinode_rw(uint32_t inum, struct dinode *d, int write)
{
    struct buf *b = bget(sb.inode_start + inum / IPB, 1);

    if (!b) {
        /* 要「寫」卻連那個區塊都讀不進來(讀取錯誤或檢查碼不符):記憶體裡的 inode 已經改了,
           磁碟上的沒有。不能假裝沒事繼續改下去 — 轉為唯讀,下次掛載時以磁碟上的為準重新檢查 */
        if (write)
            fs_failed = 1;
        return -E_IO;
    }
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
    n->d.size = (uint32_t)n->in.size;
    n->d.mtime = (uint32_t)n->in.mtime;
    n->d.mode = (uint16_t)(DMODE_VALID | (n->in.mode & S_PERM_MASK));
    n->d.uid = (uint16_t)n->in.uid;
    n->d.gid = (uint16_t)n->in.gid;
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
        n->in.mtime = n->d.mtime;
        n->in.uid = n->d.uid;
        n->in.gid = n->d.gid;
        if (n->d.mode & DMODE_VALID)             /* 沒有記錄的(舊資料)保留 inode_init 給的預設值 */
            n->in.mode = n->d.mode & S_PERM_MASK;
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
        d.parent = (uint16_t)parent;
        d.mode = (uint16_t)(DMODE_VALID | (type == I_DIR ? 0755u : 0644u));
        d.mtime = (uint32_t)timer_epoch(NULL);
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
/*
 * 檔案的第 fb 個區塊在磁碟上的編號。alloc = 1:還沒配置的話配置一個。
 *
 * 回傳 0 有兩種完全不同的意思,由 *err 區分(呼叫者一定要看):
 *   *err == 0       那個位置本來就沒有配置區塊(空洞,讀起來是 0),或 alloc 時磁碟滿了
 *   *err == -E_IO   不知道 — 間接區塊讀不到,或磁碟上記的編號不合理(內容損壞)
 * 把第二種當成第一種,就是把「讀不到」悄悄變成「內容是 0」交給程式(Codex 以主機端的測試證實過)。
 */
static uint32_t bmap(struct wnode *n, uint32_t fb, int alloc, int *err)
{
    uint32_t no;

    *err = 0;
    if (fb < NDIRECT) {
        no = n->d.addr[fb];
        if (no == 0 && alloc && (no = balloc()) != 0) {
            struct buf *z = bget(no, 0);         /* 新的區塊一定清 0:不可以讓人讀到磁碟上舊的內容 */
            if (!z) {
                bfree(no);
                *err = -E_IO;
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
                *err = -E_IO;
                return 0;
            }
            z->dirty = 1;
            bput(z);
            n->d.indirect = no;
        }
        if (n->d.indirect < sb.data_start || n->d.indirect >= sb.nblocks) {
            *err = -E_IO;                        /* inode 裡記的間接區塊編號不合理 */
            return 0;
        }
        struct buf *ib = bget(n->d.indirect, 1);
        if (!ib) {
            *err = -E_IO;                        /* 讀不到:不知道那個位置有沒有資料 */
            return 0;
        }
        uint32_t *tab = (uint32_t *)ib->data;
        no = tab[fb - NDIRECT];
        if (no == 0 && alloc && (no = balloc()) != 0) {
            struct buf *z = bget(no, 0);
            if (!z) {
                bfree(no);
                no = 0;
                *err = -E_IO;
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
    if (no != 0 && (no < sb.data_start || no >= sb.nblocks)) {
        *err = -E_IO;                            /* 編號不是 0 卻不在資料區:損壞,不是空洞 */
        return 0;
    }
    return no;
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
        int berr;
        uint32_t no = bmap(n, fb, 0, &berr);
        if (berr)
            return done ? (long)done : berr;     /* 讀不到的地方不可以當成空洞補 0 */
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
        int berr;
        uint32_t no = bmap(n, fb, 1, &berr);
        if (berr) {
            err = berr;
            break;
        }
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
    struct buf *ib = NULL;

    /* 間接區塊要先讀得進來才動手:讀不到的話,不知道它底下有哪些資料區塊,
       這時候把它本身釋放掉,那些資料區塊就永遠沒有人記得了。寧可整個操作失敗、什麼都不改 */
    if (n->d.indirect && !(ib = bget(n->d.indirect, 1)))
        return -E_IO;
    /* 留下的最後一個區塊中,超出新大小的部分清 0:之後檔案再變大時,那裡應該讀到 0。
       這也要在釋放任何東西之前做:讀不到那個區塊就整個失敗,不能留下「縮短了、但尾巴還是舊資料」 */
    if (size % BSIZE) {
        int berr;
        uint32_t no = bmap(n, keep - 1, 0, &berr);
        struct buf *b = no ? bget(no, 1) : NULL;
        if (berr || (no && !b)) {
            if (ib)
                bput(ib);
            return -E_IO;
        }
        if (b) {
            memset(b->data + size % BSIZE, 0, BSIZE - (size_t)(size % BSIZE));
            b->dirty = 1;
            bput(b);
        }
    }
    for (uint32_t fb = keep; fb < NDIRECT; fb++) {
        if (n->d.addr[fb]) {
            bfree(n->d.addr[fb]);
            n->d.addr[fb] = 0;
        }
    }
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
        if (keep <= NDIRECT) {
            bfree(n->d.indirect);
            n->d.indirect = 0;
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
        int berr;
        uint32_t no = bmap(dir, (uint32_t)(slot / DPB), 0, &berr);
        if (berr)
            return berr;                         /* 目錄的這一塊讀不到:不可以當成「這裡沒有項目」 */
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
    n = fs_failed ? NULL : ialloc(type, d->inum);
    if (fs_failed) {
        err = -E_IO;                             /* 唯讀:見檔頭「寫入失敗之後」 */
    } else if (!n) {
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

/* 加一筆紀錄指向已經存在的 inode(改名、硬連結);磁碟上的名稱數跟著加一 */
static int wfs_link(struct inode *dir, const char *name, struct inode *in)
{
    struct wnode *d = wn(dir), *n = wn(in);

    mutex_lock(&wfs_lk);
    int err = fs_failed ? -E_IO : n->d.nlink < 0xFFFF ? dir_add(d, name, n->inum) : -E_NOSPC;
    if (err == 0) {
        n->d.nlink++;
        if (in->type == I_DIR)
            n->d.parent = d->inum;               /* 目錄搬家:上層目錄改成新的 */
        err = iupdate(n);
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
    if (fs_failed) {
        mutex_unlock(&wfs_lk);
        return -E_IO;
    }
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
            /* 兩個 inode 都要寫得進去才算成功(失敗時名稱已經從目錄拿掉了,但還沒寫回磁碟:
               下面的 bsync 會把檔案系統轉為唯讀,下次掛載的檢查以磁碟上的為準) */
            if (iupdate(n) != 0 || iupdate(wn(dir)) != 0)
                err = -E_IO;
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
    long r = fs_failed ? -E_IO : file_write(wn(in), buf, n, off);
    mutex_unlock(&wfs_lk);
    return r;
}

static int wfs_truncate(struct inode *in, uint64_t size)
{
    mutex_lock(&wfs_lk);
    int r = fs_failed ? -E_IO : size <= in->size ? file_truncate(wn(in), size) : -E_INVAL;
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
    if (fs_failed) {
        n->loaded = 0;                           /* 唯讀:磁碟上的留給下次掛載的檢查去釋放 */
        mutex_unlock(&wfs_lk);
        return;
    }
    /* 內容釋放不了(間接區塊讀不到)或 inode 寫不回去:不要把它標成空的 —
       那會讓它底下的區塊永遠收不回來。留在磁碟上,轉為唯讀,交給下次掛載的檢查處理 */
    if (file_truncate(n, 0) != 0 || (n->d.type = 0, dinode_rw(n->inum, &n->d, 1)) != 0) {
        fs_failed = 1;
        n->loaded = 0;
        mutex_unlock(&wfs_lk);
        return;
    }
    n->loaded = 0;
    free_inodes++;
    if (n->inum < ialloc_hint)
        ialloc_hint = n->inum;
    bsync();
    mutex_unlock(&wfs_lk);
}

/* uid、gid、mode 被 VFS 改過了:存進磁碟上的 inode */
static int wfs_setattr(struct inode *in)
{
    mutex_lock(&wfs_lk);
    int r = fs_failed ? -E_IO : iupdate(wn(in));
    if (bsync() != 0 && r == 0)
        r = -E_IO;
    mutex_unlock(&wfs_lk);
    return r;
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

/*
 * 把修改寫回磁碟並回報結果(fsync 與關閉檔案時)。目前的粒度是「整個檔案系統」:
 * 資料、inode、位元圖一起寫回(順序見 bsync_opt);durable 時再加上檢查碼表、superblock 與裝置的 flush。
 * 寫入失敗過(fs_failed)之後一律回報 -E_IO,而且不會因為之後某一次剛好沒有東西要寫就變回成功 —
 * 那個錯誤可能有別的呼叫者還沒看到。
 */
static int wfs_flush(struct inode *in, int durable)
{
    (void)in;
    mutex_lock(&wfs_lk);
    int r = bsync_opt(durable);
    mutex_unlock(&wfs_lk);
    return r;
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
    .link = wfs_link,
    .readdir = wfs_readdir,
    .is_empty = wfs_is_empty,
    .read = wfs_read,
    .write = wfs_write,
    .truncate = wfs_truncate,
    .closed = wfs_closed,
    .release = wfs_release,
    .setattr = wfs_setattr,
    .statfs = wfs_statfs,
    .sync = wfs_sync_op,
    .flush = wfs_flush,
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
    /* 位元圖最後一個區塊中超出磁碟大小的位元,格式化時標記為使用中(見 wfs_format):
       重新算的這一份也要照做,否則全新的磁碟第一次檢查就會被當成「位元圖不符」 */
    for (uint32_t no = sb.nblocks; no < sb.bitmap_blocks * BSIZE * 8; no++)
        c.bitmap[no / 8] |= (uint8_t)(1u << (no % 8));
    if (csum_on)                             /* 最後一個區塊是 superblock 的備份 */
        c.bitmap[(sb.nblocks - 1) / 8] |= (uint8_t)(1u << ((sb.nblocks - 1) % 8));

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
            if (dinode_rw(inum, &d, 1) != 0)     /* 修不進去:檢查失敗,不要假裝已經修好 */
                goto out;
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
            if (dinode_rw(inum, &d, 1) != 0)
                goto out;
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
static void layout(struct wsuper *s, uint32_t nblocks, uint32_t ninodes, int v2)
{
    memset(s, 0, sizeof(*s));
    s->magic = v2 ? WFS_MAGIC : WFS_MAGIC1;
    s->nblocks = nblocks;
    s->ninodes = ninodes;
    s->bitmap_start = 1;
    s->bitmap_blocks = (nblocks + BSIZE * 8 - 1) / (BSIZE * 8);
    s->inode_start = s->bitmap_start + s->bitmap_blocks;
    if (v2) {                                /* 檢查碼表夾在位元圖與 inode 表之間 */
        s->csum_start = s->inode_start;
        s->csum_blocks = (nblocks * 4 + BSIZE - 1) / BSIZE;
        s->inode_start = s->csum_start + s->csum_blocks;
    }
    s->inode_blocks = (ninodes + IPB - 1) / IPB;
    s->data_start = s->inode_start + s->inode_blocks;
}

/* 配置檢查碼表在記憶體裡的複本(大小依 sb.csum_blocks) */
static int csum_alloc(void);

static int csum_alloc_impl(void)
{
    if (sb.csum_blocks == 0 || sb.csum_blocks > sizeof(csum_dirty))
        return -E_INVAL;
    csum = pmm_alloc_pages(sb.csum_blocks);
    return csum ? 0 : -E_NOMEM;
}

static int csum_alloc(void)
{
    return csum_alloc_impl();
}

/* superblock 的內容是否可信:記號、檢查碼(WFS2)、大小,以及各區的位置必須和依大小算出來的完全一致 */
/* 磁碟至少要有幾個區塊:檔案系統本身至少 64 個(sb_valid 的要求),另外留 2 個給黑盒子。
   格式化、探測、掛載用同一個門檻 — 否則會格式化出一個自己下次不認得的檔案系統 */
#define WFS_MIN_DEV_BLOCKS 66u

static int sb_valid(const struct wsuper *s, uint64_t dev_blocks)
{
    struct wsuper want;
    int v2 = s->magic == WFS_MAGIC;

    if ((!v2 && s->magic != WFS_MAGIC1) || s->nblocks < 64 || s->nblocks > dev_blocks ||
        s->ninodes < IPB || s->ninodes > MAX_INODES)
        return 0;
    if (v2 && s->crc != sb_crc(s))
        return 0;
    layout(&want, s->nblocks, s->ninodes, v2);
    return s->bitmap_start == want.bitmap_start && s->bitmap_blocks == want.bitmap_blocks &&
           s->csum_start == want.csum_start && s->csum_blocks == want.csum_blocks &&
           s->inode_start == want.inode_start && s->inode_blocks == want.inode_blocks &&
           s->data_start == want.data_start && s->data_start + 8 <= s->nblocks;
}

/* 沒有正常關機之後:檢查碼表可能比資料舊。依磁碟上實際的內容重新計算整張表 */
static int csum_rebuild(void)
{
    for (uint32_t no = 1; no < sb.nblocks; no++) {
        if (!covered(no))
            continue;
        int used = no < sb.data_start ? 1 : bitmap_op(no, 0, 0);
        if (used < 0)
            return -E_IO;
        if (!used) {
            csum_set(no, 0);
            continue;
        }
        /* 快取裡有還沒寫回的新內容時,它寫回的時候自然會更新;這裡算磁碟上的 */
        if (disk->rw(disk, (uint64_t)no * BSECT, BSECT, scratch, 0) != 0)
            return -E_IO;
        csum_set(no, block_crc(scratch));
    }
    return 0;
}

/* 在空白的磁碟上建立一個空的檔案系統 */
static int wfs_format(uint32_t nblocks)
{
    uint32_t ninodes = nblocks / 8;              /* 平均每 32KB 一個檔案 */
    struct dinode root;

    ninodes = ninodes < 64 ? 64 : ninodes > MAX_INODES ? MAX_INODES : ninodes;
    ninodes = (ninodes + IPB - 1) / IPB * IPB;
    layout(&sb, nblocks, ninodes, 1);
    if (sb.data_start + 8 > nblocks || csum_alloc() != 0)
        return -E_NOSPC;                         /* 磁碟太小(或記憶體不足) */
    memset(csum, 0, (size_t)sb.csum_blocks * BSIZE);
    memset(csum_dirty, 1, sb.csum_blocks);       /* 整張表都要寫到磁碟上 */
    csum_on = 1;

    for (uint32_t no = 1; no < sb.data_start; no++) {    /* 位元圖與 inode 表全部清 0 */
        if (no >= sb.csum_start && no < sb.csum_start + sb.csum_blocks)
            continue;                            /* 檢查碼表不經過快取,由 csum_flush 寫 */
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
    if (bitmap_op(nblocks - 1, 1, 1) < 0)        /* 最後一個區塊:superblock 的備份 */
        return -E_IO;
    memset(&root, 0, sizeof(root));
    root.type = I_DIR;
    root.nlink = 1;
    root.parent = ROOT_INUM;
    root.mode = DMODE_VALID | 0777u;             /* 磁碟的根目錄:每個使用者都可以在裡面建立自己的東西 */
    sb.clean = 1;
    if (dinode_rw(ROOT_INUM, &root, 1) != 0 || sb_write() != 0)
        return -E_IO;
    return bsync();
}

/*
 * 不掛載,只看磁碟上的檔案系統佔了幾個區塊(黑盒子要在掛載之前就知道自己那 8KB 有沒有被佔用)。
 * 回傳:區塊數;全新的磁碟回傳「格式化之後會是多少」;認不得的內容回傳 0(呼叫者應視為「不可以碰」)。
 */
uint32_t wfs_probe(struct blkdev *dev)
{
    uint64_t dev_blocks = dev->sectors / BSECT;
    struct wsuper s;
    uint32_t result = 0;
    uint8_t *buf;

    if (dev_blocks < WFS_MIN_DEV_BLOCKS || !(buf = pmm_alloc()))
        return 0;
    if (dev_blocks > 0x100000)
        dev_blocks = 0x100000;
    uint64_t fs_max = dev_blocks - 2;
    if (dev->rw(dev, 0, BSECT, buf, 0) == 0) {
        int blank = 1;
        for (unsigned i = 0; i < BSIZE && blank; i++)
            blank = buf[i] == 0;
        memcpy(&s, buf, sizeof(s));
        if (sb_valid(&s, dev_blocks)) {
            result = s.nblocks;
        } else if (dev->rw(dev, (fs_max - 1) * BSECT, BSECT, buf, 0) == 0) {
            memcpy(&s, buf, sizeof(s));
            if (s.magic == WFS_MAGIC && s.nblocks == fs_max && sb_valid(&s, dev_blocks))
                result = s.nblocks;          /* 區塊 0 壞了但備份在:掛載時會用它 */
            else if (blank)
                result = (uint32_t)fs_max;   /* 全新的磁碟 */
        }
    }
    pmm_free(buf);
    return result;
}

int wfs_mount(struct blkdev *dev, struct inode **root_out)
{
    int err = -E_IO, formatted = 0;
    uint64_t dev_blocks = dev->sectors / BSECT;

    mutex_init(&wfs_lk);
    if (mounted)
        return -E_BUSY;
    if (dev_blocks < WFS_MIN_DEV_BLOCKS)
        return -E_NOSPC;                         /* 磁碟太小 */
    if (dev_blocks > 0x100000)
        dev_blocks = 0x100000;                   /* 最多使用前 4GB */

    /* 快取的緩衝區與 inode 表:現在一次配置好,之後的操作都不再配置記憶體 */
    size_t node_pages = (MAX_INODES * sizeof(struct wnode) + PAGE_SIZE - 1) / PAGE_SIZE;
    int cache = bcache_init(dev, &sb.nblocks, &cache_ops) == 0;
    nodes = pmm_alloc_pages(node_pages);
    if (!cache || !nodes) {
        err = -E_NOMEM;
        goto fail;
    }
    memset(nodes, 0, node_pages * PAGE_SIZE);

    scratch = pmm_alloc();
    if (!scratch) {
        err = -E_NOMEM;
        goto fail;
    }
    mutex_lock(&wfs_lk);
    disk = dev;
    csum_on = 0;
    csum_verify = 0;
    csum = NULL;
    sb_restored = 0;
    csum_errors = scrub_errors = scrub_blocks = 0;
    memset(csum_dirty, 0, sizeof(csum_dirty));

    /* 最後 2 個區塊(8KB)留給黑盒子(blackbox.c),不屬於檔案系統 */
    uint64_t fs_max = dev_blocks - 2;
    /* 讀出兩份 superblock:區塊 0,以及(如果磁碟是 WFS2 而且佔滿可用範圍)最後一個區塊的備份 */
    struct wsuper primary, backup;
    int blank = 1;
    if (disk->rw(disk, 0, BSECT, scratch, 0) != 0)
        goto fail_locked;
    memcpy(&primary, scratch, sizeof(primary));
    for (unsigned i = 0; i < BSIZE && blank; i++)
        blank = scratch[i] == 0;
    if (disk->rw(disk, (fs_max - 1) * BSECT, BSECT, scratch, 0) != 0)
        goto fail_locked;
    memcpy(&backup, scratch, sizeof(backup));
    int backup_ok = backup.magic == WFS_MAGIC && backup.nblocks == fs_max && sb_valid(&backup, dev_blocks);

    err = -E_INVAL;
    if (sb_valid(&primary, dev_blocks)) {
        sb = primary;
    } else if (backup_ok) {
        /* 區塊 0 壞了(或被清掉了),但備份完好:用備份,稍後把區塊 0 寫回去。
           這個判斷要在「全空白就格式化」之前:否則區塊 0 被清成 0 的磁碟會被當成新的而整個重來 */
        sb = backup;
        sb_restored = 1;
        kprintf("disk: the primary superblock is damaged; using the backup copy and repairing it\n");
    } else if (blank) {
        /* 全新的磁碟(開頭全是 0,也沒有備份):建立檔案系統。
           開頭有別的內容時絕不格式化 — 那可能是別人的資料 */
        sb.nblocks = (uint32_t)fs_max;
        err = wfs_format((uint32_t)fs_max);
        if (err)
            goto fail_locked;
        formatted = 1;
    } else {
        goto fail_locked;
    }
    if (!formatted && sb.magic == WFS_MAGIC) {
        /* 載入檢查碼表。從這裡起,讀進來的每個區塊都會被核對 */
        err = csum_alloc();
        if (err)
            goto fail_locked;
        err = -E_IO;
        for (uint32_t i = 0; i < sb.csum_blocks; i++)
            if (disk->rw(disk, (uint64_t)(sb.csum_start + i) * BSECT, BSECT,
                         (uint8_t *)csum + (size_t)i * BSIZE, 0) != 0)
                goto fail_locked;
        csum_on = 1;
    }
    if (sb_restored)
        sb.clean = 0;                            /* 發生過損壞:這次掛載做一次完整的檢查 */

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
        if (csum_on && (csum_rebuild() != 0 || bsync_opt(1) != 0)) {
            err = -E_IO;
            goto fail_locked;
        }
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
    csum_verify = csum_on;                       /* 表格現在和磁碟一致(剛載入,或剛重算完) */
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
    if (scratch)
        pmm_free(scratch);
    if (csum)
        pmm_free_pages(csum, sb.csum_blocks);
    scratch = NULL;
    csum = NULL;
    csum_on = 0;
    if (cache)
        bcache_free();
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
    if (!fs_failed) {                            /* 寫入失敗過:不記「正常卸載」,下次掛載時檢查 */
        sb.clean = 1;
        sb_write();
        bsync_opt(1);
    }
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
    st->failed = fs_failed;
    st->checksums = csum_on;
    st->csum_errors = csum_errors;
    st->scrub_errors = scrub_errors;
    st->scrub_blocks = scrub_blocks;
    st->sb_restored = sb_restored;
    bcache_stats(&st->disk_reads, &st->disk_writes);
    mutex_unlock(&wfs_lk);
}

/*
 * 巡檢:從第 *pos 個區塊起,直接從磁碟讀 count 個區塊,逐一核對檢查碼;*pos 往後移(到尾端繞回 1)。
 * 回傳這一批裡不符的數目;沒有檢查碼的檔案系統回傳 0。不經過快取:要看的是磁碟上現在的樣子。
 */
unsigned wfs_scrub(uint32_t *pos, unsigned count)
{
    unsigned bad = 0;

    if (!mounted || !csum_on)
        return 0;
    mutex_lock(&wfs_lk);
    for (unsigned i = 0; i < count; i++) {
        uint32_t no = *pos;
        *pos = no + 1 >= sb.nblocks ? 1 : no + 1;
        if (no == 0 || no >= sb.nblocks || !covered(no) || !csum[no])
            continue;                            /* 沒有內容(或不在表裡)的不必看 */
        if (disk->rw(disk, (uint64_t)no * BSECT, BSECT, scratch, 0) != 0)
            continue;                            /* 讀不到是另一種錯誤,由一般的讀取路徑回報 */
        scrub_blocks++;
        if (csum_verify && csum[no] != block_crc(scratch)) {
            bad++;
            if (scrub_errors++ < 3)
                kprintf("disk: scrub found block %u corrupted (checksum mismatch)\n", no);
        }
    }
    mutex_unlock(&wfs_lk);
    return bad;
}

uint32_t wfs_block_count(void)
{
    return mounted ? sb.nblocks : 0;
}

/* 測試用:檔案 in 的第 fb 個區塊在磁碟上的編號(0 = 沒有配置);
   fb 為 0xFFFFFFFF 時回傳它的間接區塊 */
uint32_t wfs_test_file_block(struct inode *in, uint32_t fb)
{
    mutex_lock(&wfs_lk);
    int berr;
    uint32_t no = fb == 0xFFFFFFFFu ? wn(in)->d.indirect : bmap(wn(in), fb, 0, &berr);
    mutex_unlock(&wfs_lk);
    return no;
}

/* 測試用:把快取裡的東西都寫回去,然後整個清空,讓之後的讀取一定要碰磁碟 */
int wfs_test_drop_cache(void)
{
    if (!mounted)
        return -E_INVAL;
    mutex_lock(&wfs_lk);
    int r = bsync();
    bcache_drop_clean();
    mutex_unlock(&wfs_lk);
    return r;
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
    if (iupdate(v) != 0)                         /* 沒弄壞成功就不能回報「已經弄壞」 */
        goto out;
    rc = bsync();
out:
    mutex_unlock(&wfs_lk);
    return rc;
}
