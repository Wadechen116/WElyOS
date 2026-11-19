/*
 * blackbox.c - 黑盒子:跨開機保留的事件紀錄(說明見 blackbox.h)
 */
#include <stdarg.h>
#include "blackbox.h"
#include "blk.h"
#include "crc32.h"
#include "kprintf.h"
#include "kstring.h"
#include "pmm.h"
#include "sync.h"
#include "timer.h"

#define BB_MAGIC   0x58424257u               /* "WBBX" */
#define BB_SECTORS 16u                       /* 8KB */
#define BB_RECORDS (BB_SECTORS * BLK_SECTOR / sizeof(struct bb_record))   /* 64 */
#define REC_PER_SECTOR (BLK_SECTOR / sizeof(struct bb_record))            /* 4 */

_Static_assert(sizeof(struct bb_record) == 128, "a black box record must be 128 bytes");

static struct blkdev *disk;
static uint64_t base_lba;                    /* 這一區的第一個磁區 */
static struct bb_record recs[BB_RECORDS];    /* 整區在記憶體裡的複本:寫一筆只需要寫回它所在的那個磁區 */
static uint32_t next_seq = 1;
static unsigned next_slot;                   /* 下一筆要寫在哪一格 */
static uint32_t this_boot;
static int enabled;
static struct mutex bb_lock = MUTEX_INIT;

static int rec_valid(const struct bb_record *r)
{
    return r->magic == BB_MAGIC && r->len <= BB_TEXT_MAX && r->type >= BB_BOOT && r->type <= BB_HEALTHY &&
           r->crc == crc32(r, sizeof(*r) - sizeof(r->crc));
}

/* 從磁碟載入整區,找出最新的一筆,決定下一筆的序號與位置 */
static void load(void)
{
    uint32_t newest = 0;
    int have = 0;

    next_seq = 1;
    next_slot = 0;
    if (disk->rw(disk, base_lba, BB_SECTORS, recs, 0) != 0) {
        memset(recs, 0, sizeof(recs));       /* 讀不到:當作空的,之後的紀錄照常寫入 */
        return;
    }
    for (unsigned i = 0; i < BB_RECORDS; i++) {
        if (!rec_valid(&recs[i])) {
            memset(&recs[i], 0, sizeof(recs[i]));    /* 沒寫過、寫到一半、或損壞:當作空的 */
            continue;
        }
        /* 序號以「環狀」的方式比較(相減後看正負):序號繞回 0 之後,0 仍然比 0xFFFFFFFF 新。
           同時存在的紀錄最多 64 筆,序號的差距遠小於 2^31,所以這樣比較一定正確 */
        if (!have || (int32_t)(recs[i].seq - newest) >= 0) {
            have = 1;
            newest = recs[i].seq;
            next_seq = newest + 1;
            next_slot = (i + 1) % BB_RECORDS;
        }
    }
}

void blackbox_init(struct blkdev *dev, uint32_t fs_blocks)
{
    enabled = 0;
    /* fs_blocks 為 0 表示「認不得這顆磁碟的內容」(見 wfs_probe):那就不是我們的磁碟,
       最後那 8KB 可能是別人的資料 — 絕對不可以寫。0 不是「檔案系統很小」的意思 */
    if (!dev || fs_blocks == 0 || dev->sectors < BB_SECTORS)
        return;
    base_lba = dev->sectors - BB_SECTORS;
    /* 檔案系統必須沒有用到最後這 8KB(第 37 步之後格式化的磁碟會留出來)。
       用到了就不啟用:寧可沒有紀錄,也不能寫壞檔案系統的資料 */
    if ((uint64_t)fs_blocks * (PAGE_SIZE / BLK_SECTOR) > base_lba)
        return;
    disk = dev;
    load();
    this_boot = 1;                           /* 開機次數由紀錄自己推算:不依賴檔案系統有沒有掛載 */
    for (unsigned i = 0; i < BB_RECORDS; i++)
        if (recs[i].magic == BB_MAGIC && recs[i].boot >= this_boot)
            this_boot = recs[i].boot == UINT32_MAX ? UINT32_MAX : recs[i].boot + 1;   /* 到頂就停住,不繞回 0 */
    enabled = 1;
}

uint32_t blackbox_boot(void)
{
    return this_boot;
}

unsigned blackbox_failed_boots(void)
{
    unsigned failed = 0;

    if (!enabled)
        return 0;
    mutex_lock(&bb_lock);
    for (unsigned k = 0, slot = next_slot; k < BB_RECORDS; k++) {
        slot = (slot + BB_RECORDS - 1) % BB_RECORDS;
        const struct bb_record *r = &recs[slot];
        if (r->magic != BB_MAGIC)
            continue;
        if (r->type == BB_HEALTHY || r->type == BB_SHUTDOWN)
            break;                           /* 那一次開機是順利的:更早的不必看 */
        if (r->type != BB_BOOT)
            continue;
        int test = 0;                        /* 故意當機的測試不算開機失敗 */
        for (unsigned i = 0; i + 6 <= r->len && !test; i++)
            test = memcmp(r->text + i, "(test)", 6) == 0;
        failed += !test;
    }
    mutex_unlock(&bb_lock);
    return failed;
}

int blackbox_enabled(void)
{
    return enabled;
}

/* 把一筆紀錄填進下一格,回傳它所在的磁區(相對於這一區的開頭) */
static unsigned fill(int type, const char *text)
{
    struct bb_record *r = &recs[next_slot];
    unsigned sector = next_slot / REC_PER_SECTOR;
    size_t len = strlen(text);

    if (len > BB_TEXT_MAX)
        len = BB_TEXT_MAX;
    memset(r, 0, sizeof(*r));
    r->magic = BB_MAGIC;
    r->seq = next_seq++;
    r->time = timer_epoch(NULL);
    r->boot = this_boot;
    r->type = (uint16_t)type;
    r->len = (uint16_t)len;
    memcpy(r->text, text, len);
    r->crc = crc32(r, sizeof(*r) - sizeof(r->crc));
    next_slot = (next_slot + 1) % BB_RECORDS;
    return sector;
}

void blackbox_log(int type, const char *fmt, ...)
{
    char text[BB_TEXT_MAX + 1];
    va_list ap;

    if (!enabled)
        return;
    va_start(ap, fmt);
    kvsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    mutex_lock(&bb_lock);
    unsigned sector = fill(type, text);
    /* 寫失敗就算了:記錄本身不可以再引起別的錯誤(磁碟壞掉時尤其如此) */
    disk->rw(disk, base_lba + sector, 1, &recs[sector * REC_PER_SECTOR], 1);
    mutex_unlock(&bb_lock);
}

void blackbox_panic(int type, const char *text)
{
    if (!enabled || !disk->write_now)
        return;
    /* 不拿 bb_lock:持有它的執行緒可能就在被停下的 CPU 上,永遠不會放開。
       其他 CPU 都已經停了,這裡是唯一還在執行的 */
    unsigned sector = fill(type, text);
    disk->write_now(disk, base_lba + sector, &recs[sector * REC_PER_SECTOR]);
}

unsigned blackbox_count(void)
{
    unsigned n = 0;

    if (!enabled)
        return 0;
    mutex_lock(&bb_lock);
    for (unsigned i = 0; i < BB_RECORDS; i++)
        n += recs[i].magic == BB_MAGIC;
    mutex_unlock(&bb_lock);
    return n;
}

int blackbox_read(unsigned index, struct bb_record *out)
{
    int rc = 1;

    if (!enabled)
        return 1;
    mutex_lock(&bb_lock);
    /* 從最新的一格往回數;空的格子(被略過的損壞紀錄)不算 */
    for (unsigned k = 0, slot = next_slot; k < BB_RECORDS; k++) {
        slot = (slot + BB_RECORDS - 1) % BB_RECORDS;
        if (recs[slot].magic != BB_MAGIC)
            continue;
        if (index-- == 0) {
            *out = recs[slot];
            rc = 0;
            break;
        }
    }
    mutex_unlock(&bb_lock);
    return rc;
}

int blackbox_test_corrupt_newest(void)
{
    if (!enabled)
        return -1;
    mutex_lock(&bb_lock);
    unsigned slot = (next_slot + BB_RECORDS - 1) % BB_RECORDS;
    unsigned sector = slot / REC_PER_SECTOR;
    recs[slot].text[0] ^= 0x04;              /* 翻轉一個位元(模擬儲存媒體上的位元錯誤) */
    int rc = disk->rw(disk, base_lba + sector, 1, &recs[sector * REC_PER_SECTOR], 1);
    load();                                  /* 重新從磁碟讀:損壞的那一筆應該被認出來並略過 */
    mutex_unlock(&bb_lock);
    return rc;
}
