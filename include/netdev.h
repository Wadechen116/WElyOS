#ifndef WELYOS_NETDEV_H
#define WELYOS_NETDEV_H
#include <stddef.h>
#include <stdint.h>

/*
 * 網路卡(network interface card)驅動程式的介面。
 * 和區塊裝置(blk.h)是同一個想法:上層只認得這幾個函式,不知道底下是哪一種硬體。
 *
 *   mac     這張網路卡的硬體位址
 *   send    送出一個完整的乙太網路訊框(14 ~ 1514 位元組)。返回時訊框已經交給硬體
 *           (內容已被複製走),呼叫者的緩衝區可以立刻重複使用。0 = 成功,負值 = 失敗
 *   recv    取出一個已經收到的訊框,複製到 buf(最多 cap 位元組)。
 *           回傳長度;0 = 目前沒有;負值 = 錯誤。不等待
 *
 * 驅動程式是輪詢式的:由一條 kernel 執行緒定期呼叫 recv,不使用中斷。
 * 每個驅動程式自己保證 send 與 recv 可以同時被不同的執行緒呼叫。
 */
struct netdev {
    const char *name;
    uint8_t mac[6];
    int (*send)(struct netdev *d, const void *frame, size_t len);
    int (*recv)(struct netdev *d, void *buf, size_t cap);
};

/* 找出這台機器的網路卡(各架構實作);沒有回傳 NULL。要在排程器啟動之後呼叫 */
struct netdev *arch_net_probe(void);

/* virtio 網路卡(kernel/virtio_net.c;RISC-V 與 AArch64):在 virtio-mmio 的各個位置中尋找 */
struct netdev *virtio_net_probe(uintptr_t base, uintptr_t stride, unsigned slots);
#endif
