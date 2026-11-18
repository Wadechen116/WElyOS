#ifndef WELYOS_UAPI_FS_H
#define WELYOS_UAPI_FS_H
/*
 * uapi/fs.h - 檔案:名稱長度的上限、inode 的類型、開啟方式、struct stat / dirent / fsinfo、權限位元
 *
 * 這個目錄(uapi = user API)放的是 kernel 與使用者程式「共用」的定義:系統呼叫的編號、
 * 經由系統呼叫傳遞的結構、兩邊都要認得的常數。兩邊引入的是同一個檔案,
 * 所以不會再有「kernel 改了、使用者端忘了跟著改」的問題。
 * 規則:這裡的檔案只能引入 <stdint.h> / <stddef.h> 與同目錄的檔案,不能依賴 kernel 內部的任何東西;
 * 結構一律用固定寬度的型別,三個架構(32 與 64 位元)的佈局才會相同。
 * 已經公開的編號與欄位只增不改。
 */
#include <stdint.h>

#define FS_NAME_MAX    59    /* 單一名稱的最大長度(不含結尾的 0) */
#define FS_PATH_MAX    255   /* 整條路徑的最大長度 */

/* inode 的類型 */
enum { I_FILE = 1, I_DIR, I_DEV, I_PIPE, I_SOCK };   /* I_SOCK:網路的 socket(沒有名稱,只以描述符存在) */

/* 開啟方式(數值沿用 Linux 的定義) */
#define O_RDONLY  0x000
#define O_WRONLY  0x001
#define O_RDWR    0x002
#define O_ACCMODE 0x003
#define O_CREAT   0x040      /* 不存在就建立 */
#define O_TRUNC   0x200      /* 開啟時把內容清空 */
#define O_APPEND  0x400      /* 每次寫入都接在檔案最後 */

#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

/* 讀取目錄得到的一筆紀錄;也是使用者程式看到的格式(固定 64 位元組) */
struct dirent {
    char name[FS_NAME_MAX + 1];
    uint32_t type;           /* I_FILE / I_DIR / I_DEV */
};

struct stat {
    uint32_t type;
    uint32_t nlink;          /* 有幾個名稱指向它 */
    uint64_t size;
    uint64_t mtime;          /* 最後一次修改的時間(1970-01-01 UTC 至今的秒數);0 = 不知道 */
    uint32_t uid, gid;       /* 擁有者與所屬群組 */
    uint32_t mode;           /* 權限位元(見下方 S_*) */
    uint32_t pad;            /* 補齊到 8 的倍數:i386 與 64 位元的架構才會一樣大 */
};

/*
 * 權限位元(數值與 Unix 相同,習慣以八進位書寫):三組各三個位元,
 * 依序是擁有者、同群組的人、其他人;每組是 讀(4)、寫(2)、執行(1)。
 *   0644 = 擁有者可讀寫,其他人只能讀        0755 = 擁有者全部,其他人可讀與執行
 * 對目錄而言:讀 = 列出內容,寫 = 在裡面建立或刪除東西,執行 = 可以經過它(走到裡面的東西)。
 */
#define S_PERM_MASK 0777u
#define PERM_R 4
#define PERM_W 2
#define PERM_X 1

/* 檔案系統的用量(statfs;也是使用者程式看到的格式) */
struct fsinfo {
    uint32_t block_size;     /* 一個區塊的位元組數 */
    uint32_t pad;
    uint64_t blocks;         /* 可以放資料的區塊總數 */
    uint64_t free_blocks;
    uint64_t inodes;         /* 最多可以有幾個檔案與目錄;0 = 沒有固定的上限 */
    uint64_t free_inodes;
    char type[8];            /* 檔案系統的名稱,例如 "ramfs"、"wfs" */
};

/* 佈局的檢查:這些結構在 32 與 64 位元的架構上必須一樣大(欄位都對齊在自然的位置上) */
_Static_assert(sizeof(struct dirent) == 64, "ABI: struct dirent");
_Static_assert(sizeof(struct stat) == 40, "ABI: struct stat");
_Static_assert(sizeof(struct fsinfo) == 48, "ABI: struct fsinfo");
#endif
