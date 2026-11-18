#ifndef WELYOS_UAPI_BLACKBOX_H
#define WELYOS_UAPI_BLACKBOX_H
/*
 * uapi/blackbox.h - 黑盒子:事件的種類與一筆紀錄的格式(說明見 kernel 的 blackbox.h)
 *
 * 這個目錄(uapi = user API)放的是 kernel 與使用者程式「共用」的定義:系統呼叫的編號、
 * 經由系統呼叫傳遞的結構、兩邊都要認得的常數。兩邊引入的是同一個檔案,
 * 所以不會再有「kernel 改了、使用者端忘了跟著改」的問題。
 * 規則:這裡的檔案只能引入 <stdint.h> / <stddef.h> 與同目錄的檔案,不能依賴 kernel 內部的任何東西;
 * 結構一律用固定寬度的型別,三個架構(32 與 64 位元)的佈局才會相同。
 * 已經公開的編號與欄位只增不改。
 */
#include <stdint.h>

/* 事件的種類 */
enum {
    BB_BOOT = 1,             /* 開機 */
    BB_SHUTDOWN,             /* 正常關機 */
    BB_PANIC,                /* kernel panic */
    BB_WATCHDOG,             /* 看門狗:某顆 CPU 沒有回應 */
    BB_FAULT,                /* 使用者程式因為例外(非法存取等)被終止 */
    BB_DISK,                 /* 磁碟寫入失敗,檔案系統轉為唯讀 */
    BB_RESTART,              /* 受監督的程式結束,被重新啟動(或放棄) */
    BB_NOTE,                 /* 其他註記 */
    BB_HEALTHY,              /* 這一次開機順利完成(安全模式用它判斷「連續幾次開機失敗」) */
};

#define BB_TEXT_MAX 100

/* 一筆紀錄(磁碟上與交給使用者程式的格式相同) */
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

/* 佈局的檢查:這些結構在 32 與 64 位元的架構上必須一樣大(欄位都對齊在自然的位置上) */
_Static_assert(sizeof(struct bb_record) == 128, "ABI: struct bb_record");
#endif
