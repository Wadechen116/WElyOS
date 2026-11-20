/*
 * virtio_blk.c - virtio 區塊裝置的驅動程式(virtio-mmio,版本 2)
 *
 * virtio 是專為虛擬機器設計的一套裝置規格:不模擬任何真實的硬體,
 * 而是讓客體(我們)與模擬器(QEMU)用一塊共用的記憶體直接交換要求,又快又簡單。
 * QEMU 的 virt 機器(RISC-V、AArch64)把 virtio 裝置放在固定的記憶體位址上(virtio-mmio)。
 *
 * 交換要求的地方稱為 virtqueue,由三個部分組成(都在我們配置的記憶體裡):
 *
 *   描述表(descriptor table)  每一格描述一塊緩衝區:位址、長度、裝置可不可以寫入、下一格是誰
 *   可用環(available ring)    我們放進去:「第 N 格開頭的這一串緩衝區,請處理」
 *   已用環(used ring)         裝置放進去:「第 N 格開頭的那一串,做完了」
 *
 * 一個磁碟要求是三塊緩衝區串在一起:
 *   [標頭:讀或寫、從第幾個磁區] -> [資料] -> [狀態:1 個位元組,裝置填 0 表示成功]
 *
 * 流程:填好三格描述 -> 編號放進可用環 -> 寫入 QueueNotify 通知裝置 -> 等已用環往前走。
 * 這裡一次只送一個要求,並以輪詢等待完成(不使用中斷),所以佇列只需要很小。
 *
 * 緩衝區的位址要給「實體位址」。kernel 是恆等對映(虛擬位址 = 實體位址),直接用指標即可。
 */
#include <stddef.h>
#include "arch.h"
#include "blk.h"
#include "fs.h"
#include "kprintf.h"
#include "kstring.h"
#include "pmm.h"
#include "sync.h"
#include "timer.h"
#include "virtio_mmio.h"

#define QSIZE 8u

struct vq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[QSIZE];
};

struct vq_used {
    uint16_t flags;
    uint16_t idx;
    struct { uint32_t id, len; } ring[QSIZE];
};

struct blk_req {
    uint32_t type;               /* 0 = 讀,1 = 寫,4 = flush */
    uint32_t reserved;
    uint64_t sector;
};

struct vblk {
    struct blkdev dev;
    volatile uint32_t *reg;
    struct vq_desc *desc;
    struct vq_avail *avail;
    volatile struct vq_used *used;
    struct blk_req *req;
    volatile uint8_t *status;
    uint16_t last_used;
    int can_flush;
    struct mutex lock;           /* 一次只送一個要求 */
};

static struct vblk the_disk;

static uint32_t rd(struct vblk *v, unsigned off)           { return v->reg[off / 4]; }
static void wr(struct vblk *v, unsigned off, uint32_t val) { v->reg[off / 4] = val; }

/* 送出一個要求並等它完成。buf 為 NULL 表示沒有資料(flush) */
static int vblk_do_request(struct vblk *v, uint32_t type, uint64_t sector, void *buf, uint32_t len);

static int vblk_request(struct vblk *v, uint32_t type, uint64_t sector, void *buf, uint32_t len)
{
    mutex_lock(&v->lock);
    int rc = vblk_do_request(v, type, sector, buf, len);
    mutex_unlock(&v->lock);
    return rc;
}

/* 緊急寫入(panic 時):不拿鎖。如果有一個要求還沒完成(送出它的 CPU 已經被停下),
   先等裝置做完它(有上限),再送出自己的 — 佇列一次只放一個要求,不能重疊 */
static int vblk_write_now(struct blkdev *d, uint64_t lba, const void *buf)
{
    struct vblk *v = (struct vblk *)d;

    if (lba >= d->sectors)
        return -E_INVAL;
    for (unsigned i = 0; i < 50000000u && v->used->idx != v->avail->idx; i++)
        ;
    v->last_used = v->used->idx;
    return vblk_do_request(v, 1, lba, (void *)(uintptr_t)buf, BLK_SECTOR);
}

static int vblk_do_request(struct vblk *v, uint32_t type, uint64_t sector, void *buf, uint32_t len)
{
    int rc = 0;

    v->req->type = type;
    v->req->reserved = 0;
    v->req->sector = sector;
    *v->status = 0xFF;

    v->desc[0] = (struct vq_desc){ (uintptr_t)v->req, sizeof(*v->req), DESC_NEXT, 1 };
    if (buf) {
        /* 讀取時資料由裝置寫入我們的緩衝區 */
        v->desc[1] = (struct vq_desc){ (uintptr_t)buf, len,
                                       (uint16_t)(DESC_NEXT | (type == 0 ? DESC_WRITE : 0)), 2 };
        v->desc[2] = (struct vq_desc){ (uintptr_t)v->status, 1, DESC_WRITE, 0 };
    } else {
        v->desc[1] = (struct vq_desc){ (uintptr_t)v->status, 1, DESC_WRITE, 0 };
    }
    v->avail->ring[v->avail->idx % QSIZE] = 0;       /* 這一串從第 0 格開始 */
    __sync_synchronize();                            /* 裝置看到 idx 改變之前,上面的內容必須都已寫入 */
    v->avail->idx++;
    __sync_synchronize();
    wr(v, R_QUEUE_NOTIFY, 0);

    /* 輪詢:等已用環往前走。設一個上限,裝置沒有回應時回報錯誤而不是永遠卡住 */
    /* 兩種上限:平常以時間計(5 秒);panic 時中斷是關的,時間不會前進,所以另外有次數的上限 */
    uint64_t t0 = timer_ticks();
    unsigned long spins = 0;
    while (v->used->idx == v->last_used) {
        if (timer_ticks() - t0 > 5000 || ++spins > 200000000ul) {
            rc = -E_IO;
            break;
        }
    }
    if (rc == 0) {
        v->last_used = v->used->idx;
        if (*v->status != 0)
            rc = -E_IO;
    }
    return rc;
}

static int vblk_rw(struct blkdev *d, uint64_t lba, unsigned count, void *buf, int write)
{
    struct vblk *v = (struct vblk *)d;

    if (lba + count > d->sectors || lba + count < lba)
        return -E_INVAL;
    return vblk_request(v, write ? 1 : 0, lba, buf, count * BLK_SECTOR);
}

static int vblk_flush(struct blkdev *d)
{
    struct vblk *v = (struct vblk *)d;

    return v->can_flush ? vblk_request(v, 4, 0, NULL, 0) : 0;
}

/* 依規格的步驟啟用裝置。成功回傳 0 */
static int vblk_setup(struct vblk *v)
{
    wr(v, R_STATUS, 0);                              /* 重置 */
    wr(v, R_STATUS, ST_ACK);
    wr(v, R_STATUS, ST_ACK | ST_DRIVER);

    /* 功能協商:只接受我們用得到的。第 32 位元(VERSION_1)是現行規格必須接受的 */
    wr(v, R_DEV_FEAT_SEL, 0);
    uint32_t feat = rd(v, R_DEV_FEAT) & F_BLK_FLUSH;
    wr(v, R_DRV_FEAT_SEL, 0);
    wr(v, R_DRV_FEAT, feat);
    wr(v, R_DRV_FEAT_SEL, 1);
    wr(v, R_DRV_FEAT, 1);
    wr(v, R_STATUS, ST_ACK | ST_DRIVER | ST_FEAT_OK);
    if (!(rd(v, R_STATUS) & ST_FEAT_OK))
        return -1;
    v->can_flush = (feat & F_BLK_FLUSH) != 0;

    wr(v, R_QUEUE_SEL, 0);
    if (rd(v, R_QUEUE_READY) || rd(v, R_QUEUE_MAX) < QSIZE)
        return -1;

    /* 佇列與要求的標頭都放在同一頁裡(各部分的對齊要求:描述表 16、可用環 2、已用環 4) */
    uint8_t *page = pmm_alloc();
    if (!page)
        return -1;
    memset(page, 0, PAGE_SIZE);
    v->desc = (struct vq_desc *)page;
    v->avail = (struct vq_avail *)(page + 0x200);
    v->used = (volatile struct vq_used *)(page + 0x400);
    v->req = (struct blk_req *)(page + 0x800);
    v->status = page + 0x820;
    v->avail->flags = 1;                             /* 不需要中斷:我們用輪詢 */

    wr(v, R_QUEUE_NUM, QSIZE);
    wr(v, R_DESC_LO, (uint32_t)(uintptr_t)v->desc);
    wr(v, R_DESC_HI, (uint32_t)((uint64_t)(uintptr_t)v->desc >> 32));
    wr(v, R_AVAIL_LO, (uint32_t)(uintptr_t)v->avail);
    wr(v, R_AVAIL_HI, (uint32_t)((uint64_t)(uintptr_t)v->avail >> 32));
    wr(v, R_USED_LO, (uint32_t)(uintptr_t)v->used);
    wr(v, R_USED_HI, (uint32_t)((uint64_t)(uintptr_t)v->used >> 32));
    wr(v, R_QUEUE_READY, 1);
    wr(v, R_STATUS, ST_ACK | ST_DRIVER | ST_FEAT_OK | ST_DRIVER_OK);

    v->dev.sectors = (uint64_t)rd(v, R_CONFIG) | ((uint64_t)rd(v, R_CONFIG + 4) << 32);
    return 0;
}

struct blkdev *virtio_blk_probe(uintptr_t base, uintptr_t stride, unsigned slots)
{
    struct vblk *v = &the_disk;

    for (unsigned i = 0; i < slots; i++) {
        v->reg = (volatile uint32_t *)(base + i * stride);
        if (rd(v, R_MAGIC) != 0x74726976u || rd(v, R_DEVICE_ID) != 2)
            continue;
        if (rd(v, R_VERSION) != 2) {
            kprintf("disk: virtio device %u uses the legacy interface (start QEMU with "
                    "-global virtio-mmio.force-legacy=false)\n", i);
            continue;
        }
        mutex_init(&v->lock, "virtio-blk", MX_DEVICE);
        v->dev.name = "virtio-blk";
        v->dev.rw = vblk_rw;
        v->dev.flush = vblk_flush;
        v->dev.write_now = vblk_write_now;
        if (vblk_setup(v) != 0) {
            wr(v, R_STATUS, ST_FAILED);
            kprintf("disk: virtio device %u could not be set up\n", i);
            continue;
        }
        return &v->dev;
    }
    return NULL;
}
