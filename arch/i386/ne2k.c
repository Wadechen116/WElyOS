/*
 * ne2k.c - NE2000 相容網路卡(ISA 介面)的驅動程式
 *
 * NE2000 是 1980 年代末的乙太網路卡,因為簡單而且被大量仿製,幾乎所有模擬器都支援。
 * 選它的理由:它接在 ISA 匯流排上,暫存器在固定的 I/O port(這裡是 0x300),
 * 不必先實作 PCI 匯流排的裝置探索,用既有的 in / out 指令就能操作。
 *
 * 卡上有一顆控制晶片(DP8390)與 16KB 的記憶體(位址 0x4000 ~ 0x7FFF,以 256 位元組為一「頁」):
 *   第 0x40 ~ 0x45 頁   傳送用:我們把要送的訊框放在這裡,再叫晶片送出
 *   第 0x46 ~ 0x7F 頁   接收用的環狀緩衝區:晶片把收到的訊框依序放進來,
 *                       每個訊框前面有 4 位元組(狀態、下一個訊框在哪一頁、長度)
 * 這塊記憶體不能直接存取,要透過「遠端 DMA」:告訴晶片位址與長度,
 * 然後從資料埠(0x310)一個字一個字讀出或寫入。
 *
 * 環狀緩衝區的兩個指標:
 *   CURR   晶片下一個要寫入的頁(晶片維護)
 *   BNRY   我們已經讀到哪裡的前一頁(我們維護);兩者相鄰表示沒有新的訊框
 *
 * 這裡以輪詢方式使用(不開啟中斷)。
 */
#include <stddef.h>
#include "fs.h"
#include "io.h"
#include "kstring.h"
#include "netdev.h"
#include "sync.h"

#define NE_BASE   0x300
#define NE_DATA   (NE_BASE + 0x10)
#define NE_RESET  (NE_BASE + 0x1F)

/* 第 0 組暫存器(CR 的最高兩個位元選擇組別) */
#define CR      (NE_BASE + 0x00)     /* 命令 */
#define PSTART  (NE_BASE + 0x01)     /* 接收緩衝區的起始頁(寫) */
#define PSTOP   (NE_BASE + 0x02)     /* 接收緩衝區的結束頁(寫) */
#define BNRY    (NE_BASE + 0x03)
#define TPSR    (NE_BASE + 0x04)     /* 要傳送的訊框從哪一頁開始(寫) */
#define TBCR0   (NE_BASE + 0x05)     /* 傳送的長度(寫) */
#define TBCR1   (NE_BASE + 0x06)
#define ISR     (NE_BASE + 0x07)     /* 事件旗標;寫 1 清除 */
#define RSAR0   (NE_BASE + 0x08)     /* 遠端 DMA 的位址 */
#define RSAR1   (NE_BASE + 0x09)
#define RBCR0   (NE_BASE + 0x0A)     /* 遠端 DMA 的長度 */
#define RBCR1   (NE_BASE + 0x0B)
#define RCR     (NE_BASE + 0x0C)     /* 接收設定(寫) */
#define TCR     (NE_BASE + 0x0D)     /* 傳送設定(寫) */
#define DCR     (NE_BASE + 0x0E)     /* 資料設定(寫) */
#define IMR     (NE_BASE + 0x0F)     /* 中斷遮罩(寫) */
/* 第 1 組 */
#define PAR0    (NE_BASE + 0x01)     /* 本機的 MAC(6 個) */
#define CURR    (NE_BASE + 0x07)
#define MAR0    (NE_BASE + 0x08)     /* 群播過濾(8 個) */

#define CR_STOP   0x21               /* 停止,沒有遠端 DMA */
#define CR_START  0x22
#define CR_RREAD  0x0A               /* 啟動中 + 遠端讀取 */
#define CR_RWRITE 0x12               /* 啟動中 + 遠端寫入 */
#define CR_TXP    0x26               /* 啟動中 + 傳送 */
#define CR_PAGE1  0x40

#define ISR_RDC   0x40               /* 遠端 DMA 完成 */
#define ISR_RST   0x80

#define TX_PAGE   0x40
#define RX_START  0x46
#define RX_STOP   0x80

static struct netdev ne_dev;
static struct mutex ne_lock;         /* 晶片一次只能做一件事:收與送共用一把鎖 */
static uint8_t next_page;            /* 下一個要讀的訊框從哪一頁開始 */

/* 設定遠端 DMA 的位址與長度,並下命令 */
static void remote_setup(uint16_t addr, uint16_t len, uint8_t cmd)
{
    outb(CR, CR_START);
    outb(ISR, ISR_RDC);
    outb(RBCR0, (uint8_t)len);
    outb(RBCR1, (uint8_t)(len >> 8));
    outb(RSAR0, (uint8_t)addr);
    outb(RSAR1, (uint8_t)(addr >> 8));
    outb(CR, cmd);
}

/* 從卡上的記憶體讀出 len 位元組(len 會進位成偶數:資料埠一次傳兩個位元組) */
static void card_read(uint16_t addr, void *buf, uint16_t len)
{
    len = (uint16_t)((len + 1) & ~1u);
    remote_setup(addr, len, CR_RREAD);
    insw(NE_DATA, buf, len / 2);
}

/* 接收緩衝區是環狀的:讀取跨過結尾時,分成兩段讀 */
static void ring_read(uint16_t addr, uint8_t *buf, uint16_t len)
{
    uint16_t end = (uint16_t)RX_STOP << 8;

    if ((uint32_t)addr + len > end) {
        uint16_t first = (uint16_t)(end - addr);     /* 頁的大小是偶數,所以 first 也是偶數 */
        card_read(addr, buf, first);
        card_read((uint16_t)RX_START << 8, buf + first, (uint16_t)(len - first));
    } else {
        card_read(addr, buf, len);
    }
}

static void ne_init_chip(const uint8_t *mac)
{
    outb(CR, CR_STOP);
    outb(DCR, 0x49);                         /* 以 16 位元傳資料、一般模式 */
    outb(RBCR0, 0);
    outb(RBCR1, 0);
    outb(RCR, 0x04);                         /* 接受給我們的與廣播的訊框 */
    outb(TCR, 0x02);                         /* 設定期間先用內部迴路:不會真的送出東西 */
    outb(PSTART, RX_START);
    outb(PSTOP, RX_STOP);
    outb(BNRY, RX_START);
    outb(ISR, 0xFF);
    outb(IMR, 0x00);                         /* 不發中斷:我們用輪詢 */
    outb(CR, CR_STOP | CR_PAGE1);
    for (int i = 0; i < 6; i++)
        outb((uint16_t)(PAR0 + i), mac[i]);
    for (int i = 0; i < 8; i++)
        outb((uint16_t)(MAR0 + i), 0);
    outb(CURR, RX_START + 1);
    outb(CR, CR_START);
    outb(TCR, 0x00);                         /* 正常傳送 */
    next_page = RX_START + 1;
}

static int ne_send(struct netdev *d, const void *frame, size_t len)
{
    static uint8_t pad[60];
    int rc = 0;

    (void)d;
    if (len < 14 || len > 1514)
        return -E_INVAL;
    mutex_lock(&ne_lock);
    /* 乙太網路的訊框最短 60 位元組(不含檢查碼),不夠的補 0。補的部分必須是 0,
       不可以是緩衝區裡剛好留著的別的資料(那會把不相干的內容送上網路) */
    size_t wire = len < 60 ? 60 : len;
    remote_setup(TX_PAGE << 8, (uint16_t)((wire + 1) & ~1u), CR_RWRITE);
    if (len < 60) {
        memset(pad, 0, sizeof(pad));
        memcpy(pad, frame, len);
        outsw(NE_DATA, pad, 30);
    } else {
        outsw(NE_DATA, frame, (unsigned)(len / 2));
        if (len & 1) {                       /* 最後落單的一個位元組:補一個 0 湊成一個字 */
            uint8_t last[2] = { ((const uint8_t *)frame)[len - 1], 0 };
            outsw(NE_DATA, last, 1);
        }
    }
    for (unsigned i = 0; i < 100000 && !(inb(ISR) & ISR_RDC); i++)
        ;
    outb(ISR, ISR_RDC);
    outb(TPSR, TX_PAGE);
    outb(TBCR0, (uint8_t)wire);
    outb(TBCR1, (uint8_t)(wire >> 8));
    outb(CR, CR_TXP);
    /* 等傳送結束(TXP 位元由晶片清除);有上限 */
    unsigned spin = 0;
    while ((inb(CR) & 0x04) && ++spin < 2000000)
        ;
    if (spin >= 2000000)
        rc = -E_IO;
    outb(ISR, 0x0A);                         /* 清除「傳送完成 / 傳送錯誤」的旗標 */
    mutex_unlock(&ne_lock);
    return rc;
}

static int ne_recv(struct netdev *d, void *buf, size_t cap)
{
    uint8_t hdr[4];
    int n = 0;

    (void)d;
    mutex_lock(&ne_lock);
    outb(CR, CR_START | CR_PAGE1);
    uint8_t curr = inb(CURR);
    outb(CR, CR_START);
    if (curr != next_page) {
        ring_read((uint16_t)next_page << 8, hdr, 4);
        uint8_t next = hdr[1];
        uint16_t total = (uint16_t)(hdr[2] | (hdr[3] << 8));     /* 含這 4 個位元組的標頭 */
        /* 標頭來自卡上的記憶體:不合理的話(晶片的狀態亂了)整個重新初始化,不照著它讀 */
        if (next < RX_START || next >= RX_STOP || total < 4 + 14 || total > 4 + 1518) {
            ne_init_chip(ne_dev.mac);
        } else {
            /* 長度只扣掉前面那 4 個位元組的標頭(第一版扣了兩次,較長的封包被截斷)。
               真實的晶片還會在最後附上 4 個位元組的檢查碼,模擬器則不會。這個驅動程式只在
               模擬器上驗證過,所以照模擬器的行為處理;協定堆疊不接受超過 1514 位元組的訊框,
               要用在真實硬體上時,必須在這裡把檢查碼扣掉(只扣一次),不是放寬上層的檢查 */
            uint16_t len = (uint16_t)(total - 4);
            if (len <= cap) {
                static uint8_t tmp[1524];    /* 讀取會進位成偶數,先讀到自己的緩衝區再複製 */
                uint16_t addr = (uint16_t)(((uint16_t)next_page << 8) + 4);
                if (addr >= (uint16_t)RX_STOP << 8)
                    addr = (uint16_t)(addr - (((uint16_t)RX_STOP - RX_START) << 8));
                ring_read(addr, tmp, len);
                memcpy(buf, tmp, len);
                n = len;
            }
            next_page = next;
            outb(BNRY, (uint8_t)(next == RX_START ? RX_STOP - 1 : next - 1));
        }
        outb(ISR, 0x11);                     /* 清除「收到訊框 / 緩衝區溢位」的旗標 */
    }
    mutex_unlock(&ne_lock);
    return n;
}

struct netdev *arch_net_probe(void)
{
    uint8_t prom[32];

    mutex_init(&ne_lock);
    /* 重置:讀出重置埠再寫回去,晶片會在 ISR 設定 RST 旗標。沒有這個反應就是沒有這張卡 */
    outb(NE_RESET, inb(NE_RESET));
    unsigned i = 0;
    for (; i < 100000 && !(inb(ISR) & ISR_RST); i++)
        ;
    if (i >= 100000)
        return NULL;
    outb(ISR, 0xFF);

    /* 卡上的 PROM(位址 0 起)存著 MAC:以 16 位元模式讀出時,每個位元組會重複一次 */
    outb(CR, CR_STOP);
    outb(DCR, 0x49);
    outb(RBCR0, 0);
    outb(RBCR1, 0);
    outb(RCR, 0x20);                         /* 監聽模式:先不接收 */
    outb(TCR, 0x02);
    card_read(0, prom, 32);
    for (int k = 0; k < 6; k++)
        ne_dev.mac[k] = prom[k * 2];

    ne_dev.name = "ne2000";
    ne_dev.send = ne_send;
    ne_dev.recv = ne_recv;
    ne_init_chip(ne_dev.mac);
    return &ne_dev;
}
