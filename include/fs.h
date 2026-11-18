#ifndef WELYOS_FS_H
#define WELYOS_FS_H
#include <stddef.h>
#include <stdint.h>
#include "sync.h"

/*
 * 檔案系統。分成三層,每一層只認得下一層的介面,可以各自替換:
 *
 *   系統呼叫(proc.c)   檔案描述符(fd)、把使用者的緩衝區與路徑搬進搬出
 *        |
 *   VFS(vfs.c)         路徑解析、開啟中的檔案(struct file)、參考計數、上鎖
 *        |               VFS(Virtual File System)是「虛擬」的:它不知道資料實際存在哪裡
 *        v
 *   實際的檔案系統      ramfs(ramfs.c):內容存在記憶體裡,根目錄 "/" 是它
 *                       wfs(wfs.c):內容存在磁碟上,掛載在 "/disk"
 *                       兩者提供同一組 inode_ops,VFS 以上分不出差別
 *
 * 核心物件:
 *   inode   代表「一個檔案或目錄本身」(它的內容、大小、類型),和名稱無關。
 *           名稱只是目錄裡指向 inode 的一筆紀錄
 *   file    代表「一次開啟」:指向一個 inode,加上讀寫位置與開啟方式。
 *           同一個 inode 可以同時被開啟很多次,各有各的讀寫位置
 *
 * 所有函式失敗時回傳負的錯誤碼(-E_*),成功回傳 0 或非負的結果。
 */

/* 錯誤碼(數值沿用 POSIX 的慣例) */
#define E_NOENT        2     /* 檔案或目錄不存在 */
#define E_SRCH         3     /* 沒有這個行程 */
#define E_INTR         4     /* 等待被打斷(等待者自己被終止了) */
#define E_IO           5     /* 讀寫失敗 */
#define E_NOEXEC       8     /* 不是合法的程式檔 */
#define E_BADF         9     /* 檔案描述符無效,或開啟方式不允許這個操作 */
#define E_CHILD        10    /* 沒有符合的子行程可以等 */
#define E_NOMEM        12    /* 記憶體不足 */
#define E_FAULT        14    /* 使用者傳來的位址不合法 */
#define E_BUSY         16    /* 現在不能做(例如行程還有其他執行緒時呼叫 exec;刪除掛載點) */
#define E_EXIST        17    /* 已經存在 */
#define E_NOTDIR       20    /* 路徑中間的某一段不是目錄 */
#define E_ISDIR        21    /* 對目錄做了只能對檔案做的事 */
#define E_INVAL        22    /* 參數不合法 */
#define E_MFILE        24    /* 這個行程開啟的檔案太多 */
#define E_TXTBSY       26    /* 檔案正被當成程式執行,不可修改 */
#define E_FBIG         27    /* 檔案太大 */
#define E_NOSPC        28    /* 磁碟沒有空間了 */
#define E_PIPE         32    /* 寫入一條已經沒有人在讀的管線 */
#define E_NAMETOOLONG  36    /* 名稱或路徑太長 */
#define E_NOTEMPTY     39    /* 目錄不是空的 */

#define FS_NAME_MAX    59    /* 單一名稱的最大長度(不含結尾的 0) */
#define FS_PATH_MAX    255   /* 整條路徑的最大長度 */

/* inode 的類型 */
enum { I_FILE = 1, I_DIR, I_DEV, I_PIPE };

/* 開啟方式(數值沿用 Linux 的定義) */
#define O_RDONLY  0x000
#define O_WRONLY  0x001
#define O_RDWR    0x002
#define O_ACCMODE 0x003
#define O_CREAT   0x040      /* 不存在就建立 */
#define O_TRUNC   0x200      /* 開啟時把內容清空 */
#define O_APPEND  0x400      /* 每次寫入都接在檔案最後 */

/* 讀取目錄得到的一筆紀錄;也是使用者程式看到的格式(固定 64 位元組) */
struct dirent {
    char name[FS_NAME_MAX + 1];
    uint32_t type;           /* I_FILE / I_DIR / I_DEV */
};

struct stat {
    uint32_t type;
    uint32_t nlink;          /* 有幾個名稱指向它 */
    uint64_t size;
};

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

struct inode;

/*
 * 實際的檔案系統要提供的操作。VFS 呼叫它們之前已經鎖住該 inode(in->lock),
 * 所以實作本身不必處理「兩條執行緒同時操作同一個 inode」。
 */
struct inode_ops {
    /* 目錄:找出名稱對應的 inode;".." 回傳上層目錄。找不到回傳 -E_NOENT */
    int  (*lookup)(struct inode *dir, const char *name, struct inode **out);
    /* 目錄:建立一個新的檔案 / 目錄 / 裝置節點 */
    int  (*create)(struct inode *dir, const char *name, int type, struct inode **out);
    /* 目錄:移除名稱。*victim 回傳被移除的 inode(VFS 負責後續的參考計數) */
    int  (*unlink)(struct inode *dir, const char *name, struct inode **victim);
    /* 目錄:從位置 *pos 起的下一筆紀錄,並把 *pos 移到它之後。0 = 成功,1 = 已經沒有了。
       位置的意義由檔案系統自己決定(從 0 開始);呼叫者只負責原樣保存 */
    int  (*readdir)(struct inode *dir, uint64_t *pos, struct dirent *out);
    /* 目錄:是否沒有任何紀錄 */
    int  (*is_empty)(struct inode *dir);
    /* 檔案:從 off 開始讀 / 寫 n 個位元組;回傳實際的數量 */
    long (*read)(struct inode *in, void *buf, size_t n, uint64_t off);
    long (*write)(struct inode *in, const void *buf, size_t n, uint64_t off);
    /* 檔案:把大小改成 size(目前只用來清空) */
    int  (*truncate)(struct inode *in, uint64_t size);
    /* 檔案:回傳存放「檔案位移 off(頁對齊)那一頁」內容的實體頁;
       沒有這樣的頁(還沒寫過、或這種檔案系統不是以頁存放)回傳 NULL。
       用來把檔案內容直接對映進行程,不必複製。可以不提供(NULL) */
    void *(*get_page)(struct inode *in, uint64_t off);
    /* 一次開啟被完全關閉了(flags 為那次開啟的方式)。管線用它得知讀端 / 寫端各剩幾個。
       可以不提供(NULL) */
    void (*closed)(struct inode *in, int flags);
    /* 沒有任何名稱、也沒有人開啟了:釋放它佔用的一切 */
    void (*release)(struct inode *in);
    /* 這個 inode 所在的檔案系統的用量。可以不提供(NULL) */
    int  (*statfs)(struct inode *in, struct fsinfo *out);
    /* 把還留在記憶體裡的修改寫到儲存裝置上(對掛載的根目錄呼叫)。可以不提供(NULL) */
    int  (*sync)(struct inode *in);
};

/* 裝置節點的操作:讀寫不經過檔案系統,直接交給驅動程式 */
struct dev_ops {
    long (*read)(void *buf, size_t n);
    long (*write)(const void *buf, size_t n);
};

struct inode {
    const struct inode_ops *ops;
    const struct dev_ops *dev;   /* 僅 I_DEV */
    int type;
    uint64_t size;
    unsigned refs;               /* 有多少「使用中的參考」(開啟、路徑解析途中);由 VFS 管理 */
    unsigned nlink;              /* 有多少名稱指向它;降到 0 且 refs 為 0 時釋放 */
    unsigned exec_count;         /* 有多少行程正在執行它(頁被共用中);大於 0 時不可修改。受 lock 保護 */
    struct mutex lock;           /* 保護內容(檔案的資料、目錄的紀錄) */
    struct inode *mounted;       /* 這個目錄是掛載點:走到這裡時改走這個(另一個檔案系統的根目錄) */
    struct inode *covered;       /* 這是一個被掛載的根目錄:它蓋住的那個目錄(".." 要從那裡往上) */
};

struct file;

/* ---- kernel 內部使用的介面(路徑是 kernel 的字串) ---- */

void vfs_init(void);                                         /* 建立根目錄與 /dev/console */
int  vfs_open(const char *path, int flags, struct file **out);
void vfs_close(struct file *f);
struct file *vfs_dup(struct file *f);                        /* 多一個參考(同一次開啟) */
long vfs_read(struct file *f, void *buf, size_t n);          /* 從目前位置讀,位置往後移 */
long vfs_write(struct file *f, const void *buf, size_t n);
long vfs_pread(struct file *f, void *buf, size_t n, uint64_t off);   /* 指定位置讀,不動目前位置 */
uint64_t vfs_size(struct file *f);
int  vfs_readdir(struct file *f, struct dirent *out);        /* 0 = 取得一筆,1 = 讀完了 */
int  vfs_mkdir(const char *path);
int  vfs_unlink(const char *path);                           /* 檔案,或空的目錄 */
int  vfs_stat(const char *path, struct stat *st);
int  vfs_mkdev(const char *path, const struct dev_ops *dev); /* 建立裝置節點 */

/*
 * 掛載(mount):把另一個檔案系統接到目錄樹上。path 必須是一個已經存在的空目錄(掛載點);
 * 之後走到這個目錄,看到的是 fsroot(那個檔案系統的根目錄)。沒有卸載:掛上就一直在。
 */
int  vfs_mount(const char *path, struct inode *fsroot);
int  vfs_statfs(const char *path, struct fsinfo *out);       /* path 所在的檔案系統的用量 */
int  vfs_sync(void);                                         /* 所有掛載的檔案系統都寫回儲存裝置 */

/*
 * 把檔案當成程式執行時使用:
 *   vfs_exec_hold    宣告「這個檔案正在被執行」。從此到 vfs_exec_release 之間,
 *                    任何人寫入或清空它都會得到 -E_TXTBSY。只接受一般檔案
 *   vfs_file_page    取得檔案某一頁內容所在的實體頁(見 inode_ops 的 get_page)。
 *                    只能在 hold 與 release 之間使用:這段期間內容不會變、頁不會被釋放
 */
int   vfs_exec_hold(struct file *f);
void  vfs_exec_release(struct file *f);
void *vfs_file_page(struct file *f, uint64_t off);

/*
 * 管線(pipe):一條單向的資料通道,一端寫入、另一端依序讀出。
 * 建立後得到兩次「開啟」:*rd 只能讀、*wr 只能寫。它沒有名稱,不在任何目錄裡;
 * 兩端都關閉後自動消失。
 *   讀取:沒有資料時等待;所有寫端都關閉後,讀完剩下的資料就得到 0(和檔案結尾相同)
 *   寫入:滿了就等待;所有讀端都關閉後,寫入得到 -E_PIPE
 */
int vfs_pipe(struct file **rd, struct file **wr);

/* 替一個 inode 建立一次開啟(管線這類沒有路徑的物件使用)。失敗回傳 NULL */
struct file *vfs_file_new(struct inode *in, int flags);

/* 實際的檔案系統建立 inode 時呼叫:填好共同的欄位 */
void inode_init(struct inode *in, const struct inode_ops *ops, int type);

/* ramfs.c:建立一個空的記憶體檔案系統,回傳它的根目錄 */
struct inode *ramfs_create_root(void);
/* ramfs 目前佔用的資料頁數(測試用:檔案刪除後應該回到原本的值) */
unsigned long ramfs_pages(void);

/*
 * wfs.c:磁碟上的檔案系統。
 *   wfs_mount     掛載 dev 上的檔案系統,*root 得到它的根目錄。磁碟是全新的(開頭全為 0)
 *                 就先建立一個空的檔案系統。回傳 0 = 掛載了原有的,1 = 剛建立的,負值 = 失敗
 *                 (磁碟上是認不得的內容時不會去動它)。只能掛載一個
 *   wfs_shutdown  關機前呼叫:寫回所有資料並記下「正常卸載」
 *   wfs_stats     目前的狀態與統計
 *   wfs_check_now / wfs_test_damage  測試用,見 wfs.c
 */
struct blkdev;
struct wfs_stats {
    int mounted;
    uint32_t blocks, free_blocks;
    uint32_t inodes, free_inodes;
    uint32_t mounts;             /* 這個檔案系統被掛載過幾次(含這一次) */
    unsigned repairs;            /* 這次掛載時修復了幾個問題 */
    unsigned long disk_reads, disk_writes;   /* 實際對磁碟讀寫的區塊數 */
};
int  wfs_mount(struct blkdev *dev, struct inode **root);
void wfs_shutdown(void);
void wfs_stats(struct wfs_stats *st);
int  wfs_check_now(void);
int  wfs_test_damage(const char *victim_name);

/* initrd.c:把開機檔案包(ustar 格式的 tar)解開到檔案系統。回傳建立的檔案數,失敗回傳負值 */
int initrd_unpack(const uint8_t *image, size_t len);
#endif
