#ifndef WELYOS_BCACHE_H
#define WELYOS_BCACHE_H
#include <stdint.h>

/*
 * bcache.h - 區塊快取(buffer cache):記憶體裡留著最近用過的磁碟區塊
 *
 * 每次讀寫都去碰磁碟太慢。快取保留最近用過的 BCACHE_NBUF 個區塊(每個 4KB);
 * 修改只先改快取裡的那一份並標記為「髒」(dirty),之後才一次寫回磁碟(bcache_sync)。
 *
 * 這一層只知道「第幾個區塊、內容是什麼」,不知道裡面放的是 inode、位元圖還是檔案的資料。
 * 檔案系統自己的事(檢查碼、寫入失敗之後轉為唯讀)透過 struct bcache_ops 的三個回呼接進來。
 * 原本這些程式碼在 kernel/wfs.c 裡,和檔案系統的邏輯混在一起(第 45 步拆出)。
 *
 * 上鎖:快取自己不上鎖。所有函式都必須在「使用它的檔案系統的鎖」之下呼叫
 * (目前只有 WFS,也就是它的 wfs_lk)。同一時間只支援一顆磁碟。
 *
 * 用法:b = bget(編號, 1) -> 讀寫 b->data,改過就設 b->dirty = 1 -> bput(b)。
 * bget 到 bput 之間那個區塊不會被換掉;沒有 bput 的話那一格就永遠不能重複使用。
 */
#define BCACHE_NBUF   64u
#define BCACHE_BSIZE  4096u

struct blkdev;

struct buf {
    uint32_t no;                 /* 第幾個區塊 */
    int valid, dirty, pins;      /* pins:正在使用中(不可以被換掉)的次數 */
    unsigned long used;          /* 最後一次使用的時間(越大越新) */
    uint8_t *data;               /* 內容(BCACHE_BSIZE 個位元組) */
};

/* 檔案系統提供的回呼;都可以是 NULL */
struct bcache_ops {
    /* 剛從磁碟讀進來的區塊:內容可信嗎?回傳 0 = 可以用;其他 = 拒絕(bget 會失敗) */
    int  (*verify)(uint32_t no, const uint8_t *data);
    /* 一個區塊成功寫回磁碟了(檔案系統可以在這裡更新它的檢查碼) */
    void (*written)(uint32_t no, const uint8_t *data);
    /* 一個區塊寫不回磁碟(檔案系統應該從此拒絕修改) */
    void (*write_failed)(uint32_t no);
};

/* 啟用:配置緩衝區。*nblocks 是合法的區塊數(超過的編號 bget 會拒絕);傳指標是因為
   檔案系統在掛載的過程中才知道這個數字。回傳 0;記憶體不足回傳 -E_NOMEM */
int  bcache_init(struct blkdev *dev, const uint32_t *nblocks, const struct bcache_ops *ops);
void bcache_free(void);          /* 釋放緩衝區(掛載失敗時) */

/*
 * 取得第 no 個區塊在快取裡的那一份,並標記為使用中(用完要 bput)。
 *   load = 1:快取裡沒有的話從磁碟讀進來
 *   load = 0:呼叫者會把整個區塊覆蓋掉,不必讀;內容先清為 0
 * 失敗(讀取錯誤、內容被 verify 拒絕、編號超出範圍、全部的格子都在使用中)回傳 NULL。
 */
struct buf *bget(uint32_t no, int load);
void bput(struct buf *b);

/* 把所有髒的區塊寫回磁碟,編號由大到小。回傳 0;有任何一塊寫不進去回傳 -E_IO
   (那一塊會被放棄,並經由 write_failed 通知檔案系統) */
int  bcache_sync(void);

/* 第 no 個區塊已經不需要了(被釋放):快取裡若有它的內容,不必寫回 */
void bcache_forget(uint32_t no);

/* 丟掉所有「乾淨、沒有人在用」的區塊,讓之後的讀取一定要碰磁碟(測試用) */
void bcache_drop_clean(void);

/* 累計的磁碟讀寫次數。bcache_count_write:檔案系統自己直接寫磁碟時也記一筆 */
void bcache_stats(unsigned long *reads, unsigned long *writes);
void bcache_count_write(void);
#endif
