#ifndef WELYOS_UAPI_ERRNO_H
#define WELYOS_UAPI_ERRNO_H
/*
 * uapi/errno.h - 錯誤碼。函式失敗時回傳負的錯誤碼(-E_*),成功回傳 0 或非負的結果
 *
 * 這個目錄(uapi = user API)放的是 kernel 與使用者程式「共用」的定義:系統呼叫的編號、
 * 經由系統呼叫傳遞的結構、兩邊都要認得的常數。兩邊引入的是同一個檔案,
 * 所以不會再有「kernel 改了、使用者端忘了跟著改」的問題。
 * 規則:這裡的檔案只能引入 <stdint.h> / <stddef.h> 與同目錄的檔案,不能依賴 kernel 內部的任何東西;
 * 結構一律用固定寬度的型別,三個架構(32 與 64 位元)的佈局才會相同。
 * 已經公開的編號與欄位只增不改。
 */

/* 錯誤碼(數值沿用 POSIX 的慣例) */
#define E_PERM         1     /* 沒有做這件事的特權(例如更改別人的檔案的權限) */
#define E_NOENT        2     /* 檔案或目錄不存在 */
#define E_SRCH         3     /* 沒有這個行程 */
#define E_INTR         4     /* 等待被打斷(等待者自己被終止了) */
#define E_IO           5     /* 讀寫失敗 */
#define E_NOEXEC       8     /* 不是合法的程式檔 */
#define E_BADF         9     /* 檔案描述符無效,或開啟方式不允許這個操作 */
#define E_CHILD        10    /* 沒有符合的子行程可以等 */
#define E_NOMEM        12    /* 記憶體不足 */
#define E_ACCES        13    /* 檔案的權限不允許這個操作 */
#define E_FAULT        14    /* 使用者傳來的位址不合法 */
#define E_BUSY         16    /* 現在不能做(例如行程還有其他執行緒時呼叫 exec;刪除掛載點) */
#define E_EXIST        17    /* 已經存在 */
#define E_NOTDIR       20    /* 路徑中間的某一段不是目錄 */
#define E_ISDIR        21    /* 對目錄做了只能對檔案做的事 */
#define E_INVAL        22    /* 參數不合法 */
#define E_MFILE        24    /* 這個行程開啟的檔案太多 */
#define E_TXTBSY       26    /* 檔案正被當成程式執行,不可修改 */
#define E_XDEV         18    /* 來源與目的不在同一個檔案系統(改名、連結不能跨檔案系統) */
#define E_FBIG         27    /* 檔案太大 */
#define E_NOSPC        28    /* 磁碟沒有空間了 */
#define E_SPIPE        29    /* 這種東西沒有「讀寫位置」(管線、裝置) */
#define E_PIPE         32    /* 寫入一條已經沒有人在讀的管線 */
#define E_NAMETOOLONG  36    /* 名稱或路徑太長 */
#define E_NETDOWN      100   /* 沒有網路(沒有網路卡) */
#define E_TIMEDOUT     110   /* 等了指定的時間,沒有等到 */
#define E_NOTEMPTY     39    /* 目錄不是空的 */
#define E_NOSYS        38    /* 這台機器不支援這個功能(例如沒有高精度時鐘) */
#endif
