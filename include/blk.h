#ifndef WELYOS_BLK_H
#define WELYOS_BLK_H
#include <stdint.h>

/*
 * 區塊裝置(block device):以固定大小的「磁區」(sector,512 位元組)為單位讀寫的儲存裝置。
 * 磁碟上的檔案系統只透過這個介面碰磁碟,不知道底下是哪一種硬體;
 * 各種磁碟的驅動程式只要提供這三樣東西:
 *
 *   sectors   容量(磁區數)
 *   rw        從第 lba 個磁區起讀或寫 count 個磁區。等到做完才返回;0 = 成功,負值 = 失敗
 *   flush     要求裝置把它自己暫存的資料真正寫到儲存媒體上(可以不提供)
 *
 * 目前的驅動程式都是「輪詢」(polling):送出要求後反覆檢查裝置是否完成,不使用中斷。
 * 同一個裝置的要求由驅動程式自己的鎖排成一次一個。
 */
#define BLK_SECTOR 512u

struct blkdev {
    const char *name;                        /* 訊息中顯示的名稱 */
    uint64_t sectors;
    int (*rw)(struct blkdev *d, uint64_t lba, unsigned count, void *buf, int write);
    int (*flush)(struct blkdev *d);
};

/* 找出這台機器的磁碟(各架構實作);沒有磁碟回傳 NULL。要在排程器啟動之後呼叫 */
struct blkdev *arch_blk_probe(void);

/* virtio 區塊裝置(kernel/virtio_blk.c;RISC-V 與 AArch64 的 QEMU virt 機器使用):
   在 base 起、每隔 stride 位元組一個、共 slots 個的 virtio-mmio 位置中尋找磁碟 */
struct blkdev *virtio_blk_probe(uintptr_t base, uintptr_t stride, unsigned slots);
#endif
