/*
 * virtio_net.c - virtio 網路卡的驅動程式(virtio-mmio,版本 2)
 *
 * 和 virtio 磁碟(virtio_blk.c)用同一種佇列,差別在於有兩條:
 *   佇列 0  接收:我們事先放一批空的緩衝區進去,裝置收到訊框就填進其中一個,放到已用環
 *   佇列 1  傳送:我們放進要送的訊框,裝置送出後放到已用環
 * 每個訊框前面有一個 12 位元組的標頭(virtio_net_hdr),描述硬體代做的檢查碼、
 * 分段等功能;這裡一項都不使用,標頭全部填 0,收到的也直接略過。
 *
 * 記憶體全部在啟用時配置好(接收緩衝區、佇列、傳送用的緩衝區),之後不再配置。
 * 接收是輪詢的:virtio_net 的 recv 只是看已用環有沒有新的東西,沒有就立刻返回。
 */
#include <stddef.h>
#include "arch.h"
#include "fs.h"
#include "kprintf.h"
#include "kstring.h"
#include "netdev.h"
#include "pmm.h"
#include "sync.h"
#include "timer.h"
#include "virtio_mmio.h"

#define F_NET_MAC   (1u << 5)        /* 裝置在設定區提供 MAC 位址 */
#define NQ          16u              /* 每條佇列的大小 */
#define HDR_LEN     12u              /* virtio_net_hdr 的長度(版本 1 的格式) */
#define BUF_SIZE    2048u            /* 一個緩衝區:標頭 + 最長的訊框(1514),取 2 的次方 */

struct nq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[NQ];
};

struct nq_used {
    uint16_t flags;
    uint16_t idx;
    struct { uint32_t id, len; } ring[NQ];
};

struct vqueue {
    struct vq_desc *desc;
    struct nq_avail *avail;
    volatile struct nq_used *used;
    uint16_t last_used;
};

struct vnet {
    struct netdev dev;
    volatile uint32_t *reg;
    struct vqueue rx, tx;
    uint8_t *rx_buf[NQ];             /* 接收緩衝區;第 i 個固定對應描述表的第 i 格 */
    uint8_t *tx_buf;                 /* 傳送用:一次只送一個訊框 */
    struct mutex rx_lock, tx_lock;
};

static struct vnet the_nic;

static uint32_t rd(struct vnet *v, unsigned off)           { return v->reg[off / 4]; }
static void wr(struct vnet *v, unsigned off, uint32_t val) { v->reg[off / 4] = val; }

/* 設定一條佇列:三個部分放在同一頁(對齊要求:描述表 16、可用環 2、已用環 4) */
static int queue_setup(struct vnet *v, unsigned index, struct vqueue *q)
{
    wr(v, R_QUEUE_SEL, index);
    if (rd(v, R_QUEUE_READY) || rd(v, R_QUEUE_MAX) < NQ)
        return -1;
    uint8_t *page = pmm_alloc();
    if (!page)
        return -1;
    memset(page, 0, PAGE_SIZE);
    q->desc = (struct vq_desc *)page;
    q->avail = (struct nq_avail *)(page + 0x400);
    q->used = (volatile struct nq_used *)(page + 0x800);
    q->last_used = 0;
    q->avail->flags = 1;                     /* 不需要中斷:我們用輪詢 */
    wr(v, R_QUEUE_NUM, NQ);
    wr(v, R_DESC_LO, (uint32_t)(uintptr_t)q->desc);
    wr(v, R_DESC_HI, (uint32_t)((uint64_t)(uintptr_t)q->desc >> 32));
    wr(v, R_AVAIL_LO, (uint32_t)(uintptr_t)q->avail);
    wr(v, R_AVAIL_HI, (uint32_t)((uint64_t)(uintptr_t)q->avail >> 32));
    wr(v, R_USED_LO, (uint32_t)(uintptr_t)q->used);
    wr(v, R_USED_HI, (uint32_t)((uint64_t)(uintptr_t)q->used >> 32));
    wr(v, R_QUEUE_READY, 1);
    return 0;
}

/* 把第 i 個接收緩衝區交給裝置(它是空的,裝置可以寫入) */
static void rx_post(struct vnet *v, unsigned i)
{
    v->rx.desc[i] = (struct vq_desc){ (uintptr_t)v->rx_buf[i], BUF_SIZE, DESC_WRITE, 0 };
    v->rx.avail->ring[v->rx.avail->idx % NQ] = (uint16_t)i;
    __sync_synchronize();
    v->rx.avail->idx++;
    __sync_synchronize();
}

static int vnet_recv(struct netdev *d, void *buf, size_t cap)
{
    struct vnet *v = (struct vnet *)d;
    int n = 0;

    mutex_lock(&v->rx_lock);
    if (v->rx.used->idx != v->rx.last_used) {
        uint32_t id = v->rx.used->ring[v->rx.last_used % NQ].id;
        uint32_t len = v->rx.used->ring[v->rx.last_used % NQ].len;
        v->rx.last_used++;
        /* id 與 len 是裝置給的:先確認在範圍內才使用(不合理的當作沒有收到,緩衝區照樣還回去) */
        if (id < NQ) {
            if (len > HDR_LEN && len <= BUF_SIZE && len - HDR_LEN <= cap) {
                n = (int)(len - HDR_LEN);
                memcpy(buf, v->rx_buf[id] + HDR_LEN, (size_t)n);
            }
            rx_post(v, id);
            wr(v, R_QUEUE_NOTIFY, 0);
        }
    }
    mutex_unlock(&v->rx_lock);
    return n;
}

static int vnet_send(struct netdev *d, const void *frame, size_t len)
{
    struct vnet *v = (struct vnet *)d;
    int rc = 0;

    if (len < 14 || len > 1514)
        return -E_INVAL;
    mutex_lock(&v->tx_lock);
    memset(v->tx_buf, 0, HDR_LEN);           /* 不要求裝置代做任何事 */
    memcpy(v->tx_buf + HDR_LEN, frame, len);
    v->tx.desc[0] = (struct vq_desc){ (uintptr_t)v->tx_buf, (uint32_t)(HDR_LEN + len), 0, 0 };
    v->tx.avail->ring[v->tx.avail->idx % NQ] = 0;
    __sync_synchronize();
    v->tx.avail->idx++;
    __sync_synchronize();
    wr(v, R_QUEUE_NOTIFY, 1);
    /* 等裝置把它取走(通常立刻完成);有上限,裝置沒有回應時回報錯誤 */
    uint64_t t0 = timer_ticks();
    while (v->tx.used->idx == v->tx.last_used) {
        if (timer_ticks() - t0 > 1000) {
            rc = -E_IO;
            break;
        }
    }
    if (rc == 0)
        v->tx.last_used = v->tx.used->idx;
    mutex_unlock(&v->tx_lock);
    return rc;
}

static int vnet_setup(struct vnet *v)
{
    wr(v, R_STATUS, 0);
    wr(v, R_STATUS, ST_ACK);
    wr(v, R_STATUS, ST_ACK | ST_DRIVER);
    wr(v, R_DEV_FEAT_SEL, 0);
    uint32_t feat = rd(v, R_DEV_FEAT) & F_NET_MAC;
    wr(v, R_DRV_FEAT_SEL, 0);
    wr(v, R_DRV_FEAT, feat);
    wr(v, R_DRV_FEAT_SEL, 1);
    wr(v, R_DRV_FEAT, 1);                    /* VERSION_1 */
    wr(v, R_STATUS, ST_ACK | ST_DRIVER | ST_FEAT_OK);
    if (!(rd(v, R_STATUS) & ST_FEAT_OK) || !(feat & F_NET_MAC))
        return -1;

    if (queue_setup(v, 0, &v->rx) != 0 || queue_setup(v, 1, &v->tx) != 0)
        return -1;
    /* 緩衝區:每頁放兩個;接收 16 個加上傳送 1 個 */
    uint8_t *mem = pmm_alloc_pages((NQ + 2) / 2);
    if (!mem)
        return -1;
    for (unsigned i = 0; i < NQ; i++)
        v->rx_buf[i] = mem + (size_t)i * BUF_SIZE;
    v->tx_buf = mem + (size_t)NQ * BUF_SIZE;
    for (unsigned i = 0; i < NQ; i++)
        rx_post(v, i);

    for (unsigned i = 0; i < 6; i++)         /* 設定區開頭的 6 個位元組是 MAC */
        v->dev.mac[i] = *((volatile uint8_t *)v->reg + R_CONFIG + i);
    wr(v, R_STATUS, ST_ACK | ST_DRIVER | ST_FEAT_OK | ST_DRIVER_OK);
    wr(v, R_QUEUE_NOTIFY, 0);                /* 告訴裝置:接收緩衝區已經放好 */
    return 0;
}

struct netdev *virtio_net_probe(uintptr_t base, uintptr_t stride, unsigned slots)
{
    struct vnet *v = &the_nic;

    for (unsigned i = 0; i < slots; i++) {
        v->reg = (volatile uint32_t *)(base + i * stride);
        if (rd(v, R_MAGIC) != 0x74726976u || rd(v, R_DEVICE_ID) != 1 || rd(v, R_VERSION) != 2)
            continue;                        /* 裝置編號 1 = 網路卡 */
        mutex_init(&v->rx_lock);
        mutex_init(&v->tx_lock);
        v->dev.name = "virtio-net";
        v->dev.send = vnet_send;
        v->dev.recv = vnet_recv;
        if (vnet_setup(v) != 0) {
            wr(v, R_STATUS, ST_FAILED);
            kprintf("net: virtio device %u could not be set up\n", i);
            continue;
        }
        return &v->dev;
    }
    return NULL;
}
