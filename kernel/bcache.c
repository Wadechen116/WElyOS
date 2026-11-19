/*
 * bcache.c - 區塊快取(介面與說明見 bcache.h)
 *
 * 換出的策略:要一個新的格子時,優先用還沒有內容的;否則用「最久沒被使用、而且沒有人正在用」的。
 * 被換出的若是髒的,先寫回磁碟。
 */
#include <stddef.h>
#include "bcache.h"
#include "blk.h"
#include "kstring.h"
#include "mmu.h"
#include "panic.h"
#include "pmm.h"
#include "uapi/errno.h"

_Static_assert(BCACHE_BSIZE == PAGE_SIZE, "one cached block is one page");
#define BSECT (BCACHE_BSIZE / BLK_SECTOR)        /* 一個區塊是幾個磁區 */

static struct buf bufs[BCACHE_NBUF];
static uint8_t *pool;                            /* 所有緩衝區的記憶體(BCACHE_NBUF 頁) */
static struct blkdev *disk;
static const uint32_t *limit;                    /* 合法的區塊數 */
static const struct bcache_ops *ops;
static unsigned long use_clock;
static unsigned long disk_reads, disk_writes;

int bcache_init(struct blkdev *dev, const uint32_t *nblocks, const struct bcache_ops *o)
{
    if (!pool && !(pool = pmm_alloc_pages(BCACHE_NBUF)))
        return -E_NOMEM;
    memset(bufs, 0, sizeof(bufs));
    for (unsigned i = 0; i < BCACHE_NBUF; i++)
        bufs[i].data = pool + (size_t)i * BCACHE_BSIZE;
    disk = dev;
    limit = nblocks;
    ops = o;
    return 0;
}

void bcache_free(void)
{
    if (pool)
        pmm_free_pages(pool, BCACHE_NBUF);
    pool = NULL;
    memset(bufs, 0, sizeof(bufs));
    disk = NULL;
}

/* 把一個區塊寫回磁碟 */
static int bwrite(struct buf *b)
{
    if (disk->rw(disk, (uint64_t)b->no * BSECT, BSECT, b->data, 1) != 0) {
        if (ops && ops->write_failed)
            ops->write_failed(b->no);
        return -E_IO;
    }
    disk_writes++;
    b->dirty = 0;
    if (ops && ops->written)
        ops->written(b->no, b->data);
    return 0;
}

struct buf *bget(uint32_t no, int load)
{
    struct buf *victim = NULL;

    if (!disk || no >= *limit)
        return NULL;
    for (struct buf *b = bufs; b < bufs + BCACHE_NBUF; b++) {
        if (b->valid && b->no == no) {
            b->pins++;
            b->used = ++use_clock;
            if (!load)
                memset(b->data, 0, BCACHE_BSIZE);
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
        /* 磁碟說讀成功了,但內容和當初寫進去的一樣嗎?由檔案系統判斷(檢查碼) */
        if (ops && ops->verify && ops->verify(no, victim->data) != 0)
            return NULL;
    } else {
        memset(victim->data, 0, BCACHE_BSIZE);
    }
    victim->no = no;
    victim->valid = 1;
    victim->dirty = 0;
    victim->pins = 1;
    victim->used = ++use_clock;
    return victim;
}

void bput(struct buf *b)
{
    if (b->pins <= 0)
        panic("bcache: buffer for block %u released more often than it was taken", b->no);
    b->pins--;
}

int bcache_sync(void)
{
    int rc = 0;

    /* 每一輪挑出編號最大的髒區塊寫回:最多 BCACHE_NBUF 輪(每一輪都讓一格不再是髒的) */
    for (unsigned round = 0; round <= BCACHE_NBUF; round++) {
        struct buf *next = NULL;
        for (struct buf *b = bufs; b < bufs + BCACHE_NBUF; b++)
            if (b->valid && b->dirty && (!next || b->no > next->no))
                next = b;
        if (!next)
            break;
        if (bwrite(next) != 0) {
            next->dirty = 0;                 /* 寫不進去:放棄這一塊,免得永遠繞不出去 */
            rc = -E_IO;
        }
    }
    return rc;
}

void bcache_forget(uint32_t no)
{
    for (struct buf *b = bufs; b < bufs + BCACHE_NBUF; b++)
        if (b->valid && b->no == no && b->pins == 0)
            b->valid = b->dirty = 0;
}

void bcache_drop_clean(void)
{
    for (struct buf *b = bufs; b < bufs + BCACHE_NBUF; b++)
        if (b->pins == 0 && !b->dirty)
            b->valid = 0;
}

void bcache_stats(unsigned long *reads, unsigned long *writes)
{
    *reads = disk_reads;
    *writes = disk_writes;
}

void bcache_count_write(void)
{
    disk_writes++;
}
