/*
 * ata.c - ATA(IDE)硬碟的驅動程式,PIO 模式
 *
 * ATA(AT Attachment)是 PC 上最傳統的硬碟介面。PIO(Programmed I/O)是其中最簡單的
 * 傳輸方式:CPU 自己用 in/out 指令,把資料一個字(16 位元)一個字地從 I/O port 搬進搬出。
 * 真實硬體上它很慢(現代系統改用 DMA 或 AHCI),但在模擬器裡差別不大,而且不需要
 * 處理 PCI 與中斷,程式最少、最不容易出錯。
 *
 * 第一組控制器(primary)的暫存器在 I/O port 0x1F0~0x1F7:
 *   0x1F0  資料(16 位元)            0x1F2  要傳幾個磁區
 *   0x1F3~0x1F5  磁區編號(LBA)的低 24 位元
 *   0x1F6  選擇磁碟,加上 LBA 的最高 4 位元
 *   0x1F7  寫入 = 命令;讀取 = 狀態(BSY 忙碌、DRQ 可以傳資料、ERR 出錯)
 * LBA(Logical Block Addressing):磁區從 0 開始依序編號。這裡用 28 位元的版本,
 * 最大 128GB,足夠使用。
 *
 * 讀取一個磁區:寫入編號與命令 0x20 -> 等 DRQ -> 從資料埠讀 256 個字。
 * 寫入一個磁區:寫入編號與命令 0x30 -> 等 DRQ -> 往資料埠寫 256 個字 -> 等 BSY 結束。
 * 這裡以輪詢等待(不使用中斷),每個等待都有上限,裝置沒有回應時回報錯誤。
 */
#include <stddef.h>
#include "blk.h"
#include "fs.h"
#include "io.h"
#include "sync.h"

#define ATA_DATA    0x1F0
#define ATA_COUNT   0x1F2
#define ATA_LBA0    0x1F3
#define ATA_LBA1    0x1F4
#define ATA_LBA2    0x1F5
#define ATA_DRIVE   0x1F6
#define ATA_CMD     0x1F7
#define ATA_CTRL    0x3F6

#define ST_ERR 0x01
#define ST_DRQ 0x08
#define ST_DF  0x20
#define ST_BSY 0x80

static struct blkdev ata_dev;
static struct mutex ata_lock;

/* 等裝置不再忙碌;need_drq 時還要等到它準備好傳資料。0 = 好了,-1 = 出錯或逾時 */
static int ata_wait(int need_drq)
{
    for (unsigned i = 0; i < 2000000; i++) {
        uint8_t s = inb(ATA_CMD);
        if (s & ST_BSY)
            continue;
        if (s & (ST_ERR | ST_DF))
            return -1;
        if (!need_drq || (s & ST_DRQ))
            return 0;
    }
    return -1;
}

static int ata_rw(struct blkdev *d, uint64_t lba, unsigned count, void *buf, int write)
{
    uint16_t *p = buf;
    int rc = 0;

    if (count == 0 || count > 255 || lba + count > d->sectors)
        return -E_INVAL;
    mutex_lock(&ata_lock);
    if (ata_wait(0) != 0) {
        rc = -E_IO;
        goto out;
    }
    outb(ATA_DRIVE, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));     /* 第一顆磁碟,LBA 模式 */
    outb(ATA_COUNT, (uint8_t)count);
    outb(ATA_LBA0, (uint8_t)lba);
    outb(ATA_LBA1, (uint8_t)(lba >> 8));
    outb(ATA_LBA2, (uint8_t)(lba >> 16));
    outb(ATA_CMD, write ? 0x30 : 0x20);
    for (unsigned s = 0; s < count; s++, p += 256) {
        if (ata_wait(1) != 0) {
            rc = -E_IO;
            goto out;
        }
        if (write)
            outsw(ATA_DATA, p, 256);
        else
            insw(ATA_DATA, p, 256);
    }
    if (write && ata_wait(0) != 0)
        rc = -E_IO;
out:
    mutex_unlock(&ata_lock);
    return rc;
}

static int ata_flush(struct blkdev *d)
{
    (void)d;
    mutex_lock(&ata_lock);
    outb(ATA_DRIVE, 0xE0);
    outb(ATA_CMD, 0xE7);                             /* FLUSH CACHE */
    int rc = ata_wait(0) == 0 ? 0 : -E_IO;
    mutex_unlock(&ata_lock);
    return rc;
}

struct blkdev *arch_blk_probe(void)
{
    uint16_t id[256];

    mutex_init(&ata_lock);
    if (inb(ATA_CMD) == 0xFF)
        return NULL;                                 /* 匯流排上什麼都沒有 */
    outb(ATA_CTRL, 0x02);                            /* 不要發中斷:我們用輪詢 */
    outb(ATA_DRIVE, 0xE0);
    outb(ATA_COUNT, 0);
    outb(ATA_LBA0, 0);
    outb(ATA_LBA1, 0);
    outb(ATA_LBA2, 0);
    outb(ATA_CMD, 0xEC);                             /* IDENTIFY:請磁碟描述自己 */
    if (inb(ATA_CMD) == 0)
        return NULL;                                 /* 沒有磁碟 */
    if (ata_wait(0) != 0 || inb(ATA_LBA1) != 0 || inb(ATA_LBA2) != 0)
        return NULL;                                 /* 不是 ATA 硬碟(例如光碟機) */
    if (ata_wait(1) != 0)
        return NULL;
    insw(ATA_DATA, id, 256);

    ata_dev.name = "ata";
    ata_dev.sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);   /* 28 位元 LBA 的磁區總數 */
    ata_dev.rw = ata_rw;
    ata_dev.flush = ata_flush;
    return ata_dev.sectors ? &ata_dev : NULL;
}
