#ifndef WELYOS_BLACKBOX_H
#define WELYOS_BLACKBOX_H
#include <stdint.h>

/*
 * 黑盒子(flight recorder):把重要的事件寫到磁碟上固定的一小塊區域,重新開機後還讀得到。
 *
 * 系統出事的時候,螢幕上的訊息隨著重新開機就消失了;要知道「上一次到底怎麼了」,
 * 必須在出事當下把它記到斷電也不會消失的地方。記錄的內容:
 *   開機、正常關機、kernel panic(含訊息)、看門狗逾時、使用者程式因例外被終止、
 *   磁碟寫入失敗、受監督的程式被重新啟動,以及程式自己留下的註記。
 *
 * 位置:磁碟最後 8KB(16 個磁區),在檔案系統使用的範圍之外。這一區不經過檔案系統 —
 * panic 的時候檔案系統的狀態不可信,也不能去拿它的鎖。
 * 格式:64 筆固定 128 位元組的紀錄,環狀使用(寫滿了就蓋掉最舊的)。
 * 每筆有序號與檢查碼(CRC-32):寫到一半斷電、或內容損壞的紀錄,讀取時會被認出來並略過。
 */
enum {
    BB_BOOT = 1,             /* 開機 */
    BB_SHUTDOWN,             /* 正常關機 */
    BB_PANIC,                /* kernel panic */
    BB_WATCHDOG,             /* 看門狗:某顆 CPU 沒有回應 */
    BB_FAULT,                /* 使用者程式因為例外(非法存取等)被終止 */
    BB_DISK,                 /* 磁碟寫入失敗,檔案系統轉為唯讀 */
    BB_RESTART,              /* 受監督的程式結束,被重新啟動(或放棄) */
    BB_NOTE,                 /* 其他註記 */
};

#define BB_TEXT_MAX 100

/* 一筆紀錄(磁碟上與交給使用者程式的格式相同;使用者端的定義在 user/usys.h) */
struct bb_record {
    uint32_t magic;
    uint32_t seq;            /* 序號:越大越新 */
    uint64_t time;           /* 當時的日期時間(1970-01-01 UTC 至今的秒數) */
    uint32_t boot;           /* 第幾次開機(檔案系統被掛載的次數) */
    uint16_t type;           /* BB_* */
    uint16_t len;            /* text 的長度 */
    char text[BB_TEXT_MAX];
    uint32_t crc;            /* 前面所有欄位的 CRC-32 */
};

struct blkdev;

/* 啟用:dev 是磁碟,fs_blocks 是檔案系統使用的區塊數(每塊 4KB)。
   檔案系統之後沒有留出那 8KB(舊格式的磁碟)、或沒有磁碟時,黑盒子停用,其他函式都安全地不做事 */
void blackbox_init(struct blkdev *dev, uint32_t fs_blocks, uint32_t boot_count);
int  blackbox_enabled(void);

/* 記下一筆(printf 的格式)。會等磁碟寫完,只能在執行緒裡呼叫(不可在中斷或持有 spinlock 時) */
void blackbox_log(int type, const char *fmt, ...);

/* panic 專用:不上鎖、不等待別人,直接把這一筆寫到磁碟。在其他 CPU 已經停下之後呼叫 */
void blackbox_panic(int type, const char *text);

/* 讀取:index 0 是最新的一筆,1 是前一筆…。0 = 取得,1 = 沒有這麼多筆 */
int  blackbox_read(unsigned index, struct bb_record *out);
unsigned blackbox_count(void);

/* 測試用:把最新的一筆在磁碟上弄壞(翻轉一個位元),然後重新從磁碟載入 */
int  blackbox_test_corrupt_newest(void);
#endif
