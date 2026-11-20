/*
 * blkfault.c - 可以故意出錯的區塊裝置(測試用)
 *
 * 驅動程式與檔案系統裡「磁碟讀寫失敗」的那些分支,平常永遠不會被執行到 —
 * 模擬器裡的磁碟不會壞。沒有被執行過的程式碼等於沒有測試過,而它們偏偏是
 * 出事時才會用到的部分。
 *
 * 這裡的做法是在真正的磁碟外面包一層:它提供同樣的 struct blkdev 介面,
 * 平常原封不動地轉給底下的裝置;測試時可以要求「接下來的讀取 / 寫入回報失敗」。
 * 檔案系統看到的就是一顆會壞的磁碟,而它完全不知道中間多了一層 —
 * 這正是區塊裝置介面存在的用途。
 *
 * 平常的代價是每次讀寫多一次函式呼叫與兩次比較,和磁碟本身的時間相比可以忽略。
 */
#include <stddef.h>
#include "blk.h"
#include "fs.h"

static struct blkdev wrapper;
static struct blkdev *real;
static volatile int fail_reads, fail_writes;     /* 不為 0:該類要求一律回報失敗 */
static volatile unsigned long injected;          /* 總共回報了幾次故意的失敗 */
static volatile uint64_t fail_write_from = UINT64_MAX;  /* 這個磁區(含)之後的寫入回報失敗 */

static int fault_rw(struct blkdev *d, uint64_t lba, unsigned count, void *buf, int write)
{
    (void)d;
    if (write ? fail_writes || lba + count > fail_write_from : fail_reads) {
        injected++;
        return -E_IO;
    }
    return real->rw(real, lba, count, buf, write);
}

static int fault_flush(struct blkdev *d)
{
    (void)d;
    if (fail_writes) {
        injected++;
        return -E_IO;
    }
    return real->flush ? real->flush(real) : 0;
}

static int fault_write_now(struct blkdev *d, uint64_t lba, const void *buf)
{
    (void)d;
    if (fail_writes || lba >= fail_write_from || !real->write_now)
        return -E_IO;
    return real->write_now(real, lba, buf);
}

struct blkdev *blk_fault_wrap(struct blkdev *dev)
{
    if (!dev)
        return NULL;
    real = dev;
    wrapper.name = dev->name;
    wrapper.sectors = dev->sectors;
    wrapper.rw = fault_rw;
    wrapper.flush = fault_flush;
    wrapper.write_now = fault_write_now;
    return &wrapper;
}

void blk_fault_set(int reads, int writes)
{
    fail_reads = reads;
    fail_writes = writes;
    fail_write_from = UINT64_MAX;
}

void blk_fault_set_write_from(uint64_t first_lba)
{
    fail_write_from = first_lba;
}

int blk_fault_flip(uint64_t lba, unsigned byte, unsigned bit)
{
    static uint8_t sector[BLK_SECTOR];

    if (!real || byte >= BLK_SECTOR || bit > 7 || real->rw(real, lba, 1, sector, 0) != 0)
        return -E_IO;
    sector[byte] ^= (uint8_t)(1u << bit);
    return real->rw(real, lba, 1, sector, 1);
}

unsigned long blk_fault_count(void)
{
    return injected;
}
