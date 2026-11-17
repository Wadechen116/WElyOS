#ifndef WELYOS_PANIC_H
#define WELYOS_PANIC_H

/*
 * kernel 發生無法恢復的錯誤時呼叫。印出訊息與 backtrace 後停機。
 * 用法與 printf 相同:panic("bad pointer %p", p);
 */
void panic(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

/* 結束 panic 流程:印完診斷資訊後呼叫。見 panic_set_exit */
void panic_halt(void) __attribute__((noreturn));

/*
 * 設定 panic 後的行為:1 = 結束模擬器並回傳失敗碼(自動化測試用,
 * 由開機參數 autoexit 啟用);0 = 永久閒置(預設,方便用 GDB 檢查現場)。
 */
void panic_set_exit(int on);

/* 斷言:條件不成立就 panic,並指出原始碼位置。用於檢查「不應該發生」的狀況 */
#define KASSERT(cond) \
    do { \
        if (!(cond)) \
            panic("assertion failed: %s (%s:%d)", #cond, __FILE__, __LINE__); \
    } while (0)
#endif
