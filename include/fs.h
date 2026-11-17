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
 *   實際的檔案系統      目前只有 ramfs(ramfs.c):內容存在記憶體裡。
 *                       之後加入磁碟上的檔案系統時,只要提供同一組 inode_ops
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
#define E_IO           5     /* 讀寫失敗 */
#define E_NOEXEC       8     /* 不是合法的程式檔 */
#define E_BADF         9     /* 檔案描述符無效,或開啟方式不允許這個操作 */
#define E_NOMEM        12    /* 記憶體不足 */
#define E_FAULT        14    /* 使用者傳來的位址不合法 */
#define E_BUSY         16    /* 現在不能做(例如行程還有其他執行緒時呼叫 exec) */
#define E_EXIST        17    /* 已經存在 */
#define E_NOTDIR       20    /* 路徑中間的某一段不是目錄 */
#define E_ISDIR        21    /* 對目錄做了只能對檔案做的事 */
#define E_INVAL        22    /* 參數不合法 */
#define E_MFILE        24    /* 這個行程開啟的檔案太多 */
#define E_TXTBSY       26    /* 檔案正被當成程式執行,不可修改 */
#define E_FBIG         27    /* 檔案太大 */
#define E_NAMETOOLONG  36    /* 名稱或路徑太長 */
#define E_NOTEMPTY     39    /* 目錄不是空的 */

#define FS_NAME_MAX    59    /* 單一名稱的最大長度(不含結尾的 0) */
#define FS_PATH_MAX    255   /* 整條路徑的最大長度 */

/* inode 的類型 */
enum { I_FILE = 1, I_DIR, I_DEV };

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
    /* 目錄:第 idx 筆紀錄。0 = 成功,1 = 已經沒有了 */
    int  (*readdir)(struct inode *dir, uint64_t idx, struct dirent *out);
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
    /* 沒有任何名稱、也沒有人開啟了:釋放它佔用的一切 */
    void (*release)(struct inode *in);
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
 * 把檔案當成程式執行時使用:
 *   vfs_exec_hold    宣告「這個檔案正在被執行」。從此到 vfs_exec_release 之間,
 *                    任何人寫入或清空它都會得到 -E_TXTBSY。只接受一般檔案
 *   vfs_file_page    取得檔案某一頁內容所在的實體頁(見 inode_ops 的 get_page)。
 *                    只能在 hold 與 release 之間使用:這段期間內容不會變、頁不會被釋放
 */
int   vfs_exec_hold(struct file *f);
void  vfs_exec_release(struct file *f);
void *vfs_file_page(struct file *f, uint64_t off);

/* 實際的檔案系統建立 inode 時呼叫:填好共同的欄位 */
void inode_init(struct inode *in, const struct inode_ops *ops, int type);

/* ramfs.c:建立一個空的記憶體檔案系統,回傳它的根目錄 */
struct inode *ramfs_create_root(void);
/* ramfs 目前佔用的資料頁數(測試用:檔案刪除後應該回到原本的值) */
unsigned long ramfs_pages(void);

/* initrd.c:把開機檔案包(ustar 格式的 tar)解開到檔案系統。回傳建立的檔案數,失敗回傳負值 */
int initrd_unpack(const uint8_t *image, size_t len);
#endif
