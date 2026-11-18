#ifndef WELYOS_VIRTIO_MMIO_H
#define WELYOS_VIRTIO_MMIO_H
#include <stdint.h>

/*
 * virtio_mmio.h - virtio-mmio(版本 2)各種裝置共用的定義
 *
 * virtio 的每種裝置(磁碟、網路卡…)用同一套暫存器與同一種佇列(virtqueue),
 * 差別只在裝置編號、功能位元與要求的格式。共用的部分放在這裡;
 * 佇列的說明見 virtio_blk.c 開頭。
 */

/* virtio-mmio 的暫存器(位移) */
#define R_MAGIC        0x000     /* 固定為 "virt" */
#define R_VERSION      0x004     /* 2 = 現行規格;1 = 舊版(legacy),這裡不支援 */
#define R_DEVICE_ID    0x008     /* 2 = 區塊裝置;0 = 這個位置沒有裝置 */
#define R_DEV_FEAT     0x010
#define R_DEV_FEAT_SEL 0x014
#define R_DRV_FEAT     0x020
#define R_DRV_FEAT_SEL 0x024
#define R_QUEUE_SEL    0x030
#define R_QUEUE_MAX    0x034
#define R_QUEUE_NUM    0x038
#define R_QUEUE_READY  0x044
#define R_QUEUE_NOTIFY 0x050
#define R_STATUS       0x070
#define R_DESC_LO      0x080
#define R_DESC_HI      0x084
#define R_AVAIL_LO     0x090
#define R_AVAIL_HI     0x094
#define R_USED_LO      0x0a0
#define R_USED_HI      0x0a4
#define R_CONFIG       0x100     /* 區塊裝置:開頭 8 個位元組是容量(磁區數) */

#define ST_ACK       1u
#define ST_DRIVER    2u
#define ST_DRIVER_OK 4u
#define ST_FEAT_OK   8u
#define ST_FAILED    128u

#define F_BLK_FLUSH  (1u << 9)   /* 裝置支援「把暫存的資料寫到媒體上」的要求 */

struct vq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};
#define DESC_NEXT  1u            /* 後面還有一格 */
#define DESC_WRITE 2u            /* 這塊緩衝區由裝置寫入(否則是裝置讀取) */

#endif
