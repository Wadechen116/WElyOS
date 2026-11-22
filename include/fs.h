#ifndef WELYOS_FS_H
#define WELYOS_FS_H
#include <stddef.h>
#include <stdint.h>
#include "sync.h"
#include "uapi/errno.h"       /* 錯誤碼 E_* */
#include "uapi/fs.h"          /* 開啟方式、struct stat / dirent / fsinfo、權限位元 */

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
    /* 目錄:多一個名稱 name 指向「已經存在」的 inode in(同一個檔案系統)。
       in 是目錄時,它的上層目錄改成 dir。改名與硬連結都靠它。可以不提供(NULL) */
    int  (*link)(struct inode *dir, const char *name, struct inode *in);
    /* 目錄:從位置 *pos 起的下一筆紀錄,並把 *pos 移到它之後。0 = 成功,1 = 已經沒有了,
       負值 = 錯誤。錯誤若只關於「這一筆」(例如它指向的 inode 讀不到),*pos 照樣移到它之後,
       呼叫者可以回報後繼續列下一筆。
       位置的意義由檔案系統自己決定(從 0 開始);呼叫者只負責原樣保存 */
    int  (*readdir)(struct inode *dir, uint64_t *pos, struct dirent *out);
    /* 目錄:是否沒有任何紀錄。1 = 空的,0 = 不是,負值 = 讀不到(不知道;呼叫者必須當成失敗,
       不可以當成「空的」或「不是空的」之一) */
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
    /* 把這個檔案還留在記憶體裡的修改寫出去,並回報有沒有成功(0 或 -E_IO)。
       durable = 1:還要確定資料已經到了儲存媒體上(fsync);0:寫給裝置就好(關閉檔案時)。
       沒有「待寫的東西」的檔案系統(記憶體檔案系統)不必提供(NULL = 一律成功) */
    int  (*flush)(struct inode *in, int durable);
    /* 沒有任何名稱、也沒有人開啟了:釋放它佔用的一切 */
    void (*release)(struct inode *in);
    /* uid、gid、mode 被 VFS 改過了:存放在別處(磁碟)的檔案系統在這裡把它們存起來。
       可以不提供(NULL) */
    int  (*setattr)(struct inode *in);
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
    uint32_t uid, gid;           /* 擁有者與所屬群組。和 mode 一樣由 VFS 管理,受 lock 保護 */
    uint16_t mode;               /* 權限位元(0777 的範圍) */
    uint64_t mtime;              /* 最後一次修改的時間(Unix 時間,秒)。由 VFS 在修改前更新;受 lock 保護 */
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
/*
 * 把檔案寫回儲存裝置,並「回報結果」。這和 vfs_close 是分開的兩件事:
 * vfs_close 只是放掉一個參考(同一次開啟可能還被 dup、fork 出來的別人拿著),沒有回傳值;
 * 需要知道資料有沒有寫成功的人,要明確地呼叫這兩個函式之一。
 *   vfs_fsync  資料確定到了儲存媒體上才回傳 0(sys_fsync)。管線、裝置、socket 回傳 -E_INVAL:
 *              它們沒有「寫回」這件事,不可以回報成功讓人誤以為資料已經保存
 *   vfs_flush  寫給裝置就好(sys_close 使用);唯讀的開啟、不是一般檔案或目錄的,回傳 0
 * 記憶體檔案系統上的檔案回傳 0:沒有待寫的東西 — 這不表示它的內容能保存到下次開機。
 */
int  vfs_fsync(struct file *f);
int  vfs_flush(struct file *f);
struct file *vfs_dup(struct file *f);                        /* 多一個參考(同一次開啟) */
long vfs_read(struct file *f, void *buf, size_t n);          /* 從目前位置讀,位置往後移 */
long vfs_write(struct file *f, const void *buf, size_t n);
long vfs_pread(struct file *f, void *buf, size_t n, uint64_t off);   /* 指定位置讀,不動目前位置 */
uint64_t vfs_size(struct file *f);
int  vfs_readdir(struct file *f, struct dirent *out);        /* 0 = 取得一筆,1 = 讀完了 */
int  vfs_mkdir(const char *path);
int  vfs_unlink(const char *path);                           /* 檔案,或空的目錄 */
/* 改變讀寫位置。whence:SEEK_SET 從頭算、SEEK_CUR 從目前位置算、SEEK_END 從結尾算。
   回傳新的位置。只有一般檔案可以(目錄只能回到開頭);管線與裝置回傳 -E_SPIPE */
long vfs_seek(struct file *f, long off, int whence);
/* 改名 / 搬移(同一個檔案系統內)。目的已經存在時回傳 -E_EXIST(不會蓋掉它) */
int  vfs_rename(const char *oldpath, const char *newpath);
/* 硬連結:讓 newpath 成為 oldpath 那個檔案的另一個名稱(只限一般檔案,同一個檔案系統) */
int  vfs_link(const char *oldpath, const char *newpath);
int  vfs_stat(const char *path, struct stat *st);
int  vfs_mkdev(const char *path, const struct dev_ops *dev); /* 建立裝置節點 */

/*
 * 權限。每個檔案與目錄有擁有者(uid)、群組(gid)與權限位元(mode);
 * VFS 在每個操作裡依「現在是誰在問」(cred.h 的 cred_current)檢查,不符合回傳 -E_ACCES。
 * kernel 自己的呼叫(cred_current 為 NULL)不受限制。
 *   vfs_access  path 是否允許 want(PERM_R / PERM_W / PERM_X 的組合)
 *   vfs_chmod   更改權限位元:只有擁有者或有 CAP_FS 的行程可以
 *   vfs_chown   更改擁有者與群組:只有有 CAP_FS 的行程可以(否則可以把檔案「送」給別人來規避配額或嫁禍)
 */
int  vfs_access(const char *path, int want);
int  vfs_chmod(const char *path, unsigned mode);
int  vfs_chown(const char *path, uint32_t uid, uint32_t gid);

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
/* 一次開啟所指的 inode(socket 這類物件用它從描述符找回自己) */
struct inode *vfs_file_inode(struct file *f);

/* 實際的檔案系統建立 inode 時呼叫:填好共同的欄位 */
void inode_init(struct inode *in, const struct inode_ops *ops, int type);

/* ramfs.c:建立一個空的記憶體檔案系統,回傳它的根目錄 */
struct inode *ramfs_create_root(void);
/* ramfs 目前佔用的資料頁數(測試用:檔案刪除後應該回到原本的值) */
unsigned long ramfs_pages(void);
/* 設定 ramfs 記帳用的 FS domain(I-240 C3.2 (d));要在建立根目錄之前呼叫,所有物件才會記帳 */
struct mb_domain;
struct mb_domain *ramfs_set_domain(struct mb_domain *d);   /* 回傳原本的 */
void ramfs_test_strict(int on);                  /* 測試用:kernel 執行緒也受 FS 上限限制 */

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
    int failed;                  /* 寫入磁碟失敗過,檔案系統已轉為唯讀 */
    int checksums;               /* 這個檔案系統的每個區塊有檢查碼(第 38 步起格式化的) */
    unsigned long csum_errors;   /* 讀取時發現檢查碼不符、因而拒絕的次數 */
    unsigned long scrub_errors;  /* 巡檢發現的損壞區塊數(累計) */
    unsigned long scrub_blocks;  /* 巡檢核對過的區塊數(累計) */
    int sb_restored;             /* 這次掛載時,superblock 是從備份救回來的 */
    unsigned long disk_reads, disk_writes;   /* 實際對磁碟讀寫的區塊數 */
    uint32_t data_start;         /* 第一個資料區塊(之前是 superblock、位元圖、inode 表) */
};
uint32_t wfs_probe(struct blkdev *dev);      /* 不掛載,只回報檔案系統佔幾個區塊(0 = 認不得的內容) */
int  wfs_mount(struct blkdev *dev, struct inode **root);
void wfs_shutdown(void);
void wfs_stats(struct wfs_stats *st);
int  wfs_check_now(void);
int  wfs_test_damage(const char *victim_name);
int  wfs_test_drop_cache(void);
int  wfs_test_unmount(int clean);            /* 測試用:卸載、釋放全部記憶體(沒有掛到 VFS 的掛載才可以) */
uint32_t wfs_test_file_block(struct inode *in, uint32_t fb);
/* 巡檢:從 *pos 起直接讀磁碟核對 count 個區塊的檢查碼;回傳不符的數目,*pos 往後移(會繞回) */
unsigned wfs_scrub(uint32_t *pos, unsigned count);
uint32_t wfs_block_count(void);

/* initrd.c:把開機檔案包(ustar 格式的 tar)解開到檔案系統。回傳建立的檔案數,失敗回傳負值 */
int initrd_unpack(const uint8_t *image, size_t len);
#endif
