#ifndef WELYOS_PROC_INTERNAL_H
#define WELYOS_PROC_INTERNAL_H
/*
 * proc_internal.h - 行程管理內部共用的定義(只給 kernel/proc*.c 使用)
 *
 * 行程管理原本是一個 2500 多行的檔案(第 47 步拆開)。這裡放的是拆開之後幾個檔案之間
 * 需要互相看到的東西:struct proc 本身、幾個常數,以及跨檔案呼叫的函式。
 * 對 kernel 其他部分公開的介面仍然是 include/proc.h;其他地方不應該引入這個檔案 —
 * 這裡的任何東西都可以隨實作改變。
 *
 *   kernel/proc.c         核心:身分、程式映像、執行緒、建立 / 等待、exec、fork、futex、行程清單
 *   kernel/proc_signal.c  訊號、暫停與繼續、訊號處理函式、返回使用者模式前的檢查
 *   kernel/proc_mem.c     page fault、和使用者空間之間的複製、記憶體配額、mmap / sbrk、故障注入
 *   kernel/proc_fs.c      檔案描述符表、目前的工作目錄
 *   kernel/syscall.c      系統呼叫層:只引入 proc_calls.h,看不到 struct proc 的內容
 */
#include <stddef.h>
#include "arch.h"
#include "blackbox.h"
#include "console.h"
#include "cpu.h"
#include "cred.h"
#include "fs.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "kstring.h"
#include "membudget.h"
#include "mmu.h"
#include "net.h"
#include "netif.h"
#include "panic.h"
#include "pmm.h"
#include "proc.h"
#include "sched.h"
#include "spinlock.h"
#include "timer.h"
#include "vm.h"

/*
 * 使用者程式的檔案格式(user/init.c 編譯後的原始二進位,放在檔案系統的 /bin/init)。
 * 開頭是一個小標頭,描述各區段的範圍(由 user/user.ld 產生):
 */
struct uimg_hdr {
    uint32_t magic;         /* "WELY" */
    uint32_t entry;         /* 進入點(相對於 USER_BASE 的偏移,以下皆同) */
    uint32_t text_end;      /* 程式碼與唯讀資料的結尾(對齊到頁) */
    uint32_t data_end;      /* 已初始化資料的結尾 = 檔案內容的結尾 */
    uint32_t bss_end;       /* 未初始化資料的結尾;data_end 之後的部分要清為 0 */
};
#define UIMG_MAGIC 0x594C4557u

/* 使用者 stack:每條執行緒一格(1MB),從 USER_STACK_TOP 往下排;每格只對映最上面 16KB,
   其餘留空,所以 stack 用過頭會碰到沒有對映的位址而被終止 */
#define USER_STACK_TOP   (USER_BASE + 0x30000000u)
#define USER_STACK_SLOT  0x100000u
#define USER_STACK_PAGES 4u
#define PROC_MAX_THREADS 16u

/* 使用者 heap:程式的資料之後、stack 之前的一段固定範圍(程式本身最大 256MB,見 image_build)。
   行程以 sbrk 移動「結尾」(break)來取得或歸還記憶體;範圍內的頁用到才配置 */
#define USER_HEAP_BASE   (USER_BASE + 0x10000000u)
#define USER_HEAP_MAX    0x10000000u            /* 最多 256MB */

/* 匿名對映(mmap):heap 之後的另一段固定範圍,每次要求得到其中一塊,用完可以個別歸還 */
#define USER_MMAP_BASE   (USER_BASE + 0x20000000u)
#define USER_MMAP_MAX    0x0F000000u            /* 240MB;其上是各執行緒的 stack */
#define PROC_MAX_MAPS    16u                    /* 每個行程最多同時有幾塊 */

/* 記憶體配額的預設值:每個行程最多可以要 32MB(heap 加上匿名對映);可以用 memlimit 調低 */
#define PROC_DEFAULT_LIMIT_PAGES (32u * 1024 * 1024 / PAGE_SIZE)

#define FUTEX_BUCKETS 16u
#define PROC_MAX_FD   16u            /* 每個行程最多同時開啟幾個檔案 */

#define NSIG    32                   /* 訊號編號的上限(1 ~ NSIG-1) */
#define SIG_DFL 0u                   /* 預設的處理方式(終止或暫停) */
#define SIG_IGN 1u                   /* 忽略 */

/*
 * ---- 行程的狀態,依「誰擁有」分成四組 ----
 *
 * struct proc 裡除了核心自己的欄位,嵌著三個子結構;每一個只有一個檔案可以存取:
 *
 *   p->sig     struct proc_sig     proc_signal.c   訊號的處理方式、待處理的訊號、暫停
 *   p->mem     struct proc_mem     proc_mem.c      記憶體的記帳(配額、heap、匿名對映)、故障注入
 *   p->files   struct proc_files   proc_fs.c       檔案描述符表、目前的工作目錄
 *   其餘欄位                        proc.c          身分、生命週期、執行緒、父子關係
 *
 * 別的模組要用到某一組的狀態,一律經由擁有者提供的函式(見 proc_calls.h)。
 * 這條規則由編譯器強制(見下面的 PSIG_F / PMEM_F / PFILES_F):在擁有者以外的檔案
 * 寫 p->sig.stopped 之類的會編譯不過。scripts/arch-check.sh 另外在回歸測試裡
 * 實際編譯一組「應該失敗」的片段,確認這道防線還在。
 * 核心的欄位(pid、pgid、cred、zombie…)其他三個模組可以讀,因為它們本來就是「關於這個行程」的事;
 * 系統呼叫層(syscall.c)則連 struct proc 的定義都看不到。
 */
/*
 * 界線由編譯器把關:只有擁有者看得到欄位真正的名稱。
 * 擁有者在引入這個檔案之前定義自己的記號(例如 proc_signal.c 定義 PROC_OWNER_SIG);
 * 其他檔案看到的欄位名稱被加上一段後綴,所以寫 p->sig.stopped 會得到
 * 「struct proc_sig 沒有名為 stopped 的成員」的編譯錯誤 — 不是靠約定,也不是靠事後的文字檢查。
 * 結構的大小與排列對所有檔案都相同(只是名稱不同),所以整個 struct proc 照常可以配置、清零、複製。
 */
#ifdef PROC_OWNER_SIG
#define PSIG_F(name) name
#else
#define PSIG_F(name) name##__only_proc_signal_c_may_use_this
#endif
#ifdef PROC_OWNER_MEM
#define PMEM_F(name) name
#else
#define PMEM_F(name) name##__only_proc_mem_c_may_use_this
#endif
#ifdef PROC_OWNER_FILES
#define PFILES_F(name) name
#else
#define PFILES_F(name) name##__only_proc_fs_c_may_use_this
#endif

struct proc_sig {
    uintptr_t PSIG_F(handler)[NSIG];         /* 每個訊號的處理方式:SIG_DFL、SIG_IGN 或使用者函式的位址 */
    uintptr_t PSIG_F(restorer);              /* 處理函式返回時跳去的地方(使用者端的一小段程式,呼叫 sigreturn) */
    uint32_t PSIG_F(pending);                /* 已經送來、還沒交給處理函式的訊號(bit n = 訊號 n) */
    int PSIG_F(stopped);                     /* 不為 0 = 被暫停了,值是造成暫停的訊號編號 */
    int PSIG_F(stop_reported);               /* 這次暫停已經告訴過父行程了(waitpid 的 WUNTRACED 只回報一次) */
    struct wait_queue PSIG_F(stop_wq);       /* 暫停期間,它的執行緒睡在這裡,等 SIGCONT */
};

struct proc_mem {
    struct mb_domain *PMEM_F(dom);           /* 記憶體配額的帳戶(持有一份參考;承諾與用量記在位址空間的帳本上) */
    unsigned long PMEM_F(kobj);              /* 行程結構本身記在 dom 上的 KOBJ 頁數 */
    struct { uintptr_t start; unsigned long pages; } PMEM_F(maps)[PROC_MAX_MAPS];   /* 匿名對映;pages 0 = 空位 */
    uintptr_t PMEM_F(mmap_next);             /* 下一塊對映要放的位址(只往上走,不重複使用) */
    uintptr_t PMEM_F(brk);                   /* heap 目前的結尾(0 = 還沒用過,等於 USER_HEAP_BASE);受 pstate_lk 保護 */
    volatile int PMEM_F(seu_pending);        /* 故障注入(SYS_seu):等著翻轉的一個位元(見 proc_mem.c) */
    struct mutex PMEM_F(mx);         /* 序列化「改變使用者範圍的對映」:fault 的配置、munmap、sbrk(見 proc_mem.c) */
    uintptr_t PMEM_F(seu_addr);
    uint8_t PMEM_F(seu_mask);
};

struct proc_files {
    struct file *PFILES_F(fd)[PROC_MAX_FD];    /* 檔案描述符表:編號 -> 開啟中的檔案;NULL = 空位 */
    char PFILES_F(cwd)[FS_PATH_MAX + 1];       /* 目前的工作目錄(整理過的絕對路徑);受 pstate_lk 保護 */
};

struct proc {
    int pid;
    int ppid;                        /* 建立它的行程(父行程);0 = 由 kernel 建立,或父行程已經結束 */
    int pgid;                        /* 行程群組:shell 把同一條管線的行程放在同一組,
                                        Ctrl-C 一次終止整組(見 console.c) */
    int detached;                    /* 父行程已經結束:沒有人會來 wait,結束時自動回收 */
    struct cred cred;                /* 身分與特權(見 cred.h);受排程器的鎖保護 */
    char name[32];                   /* 程式檔的名稱(路徑的最後一段),訊息中使用 */
    struct vm_space *space;          /* 位址空間 */
    struct file *exe;                /* 正在執行的程式檔。一直開著:它的頁直接對映在行程裡 */
    unsigned nthreads;               /* 還活著的執行緒數 */
    unsigned nstacks;                /* 已分配的 stack 格數(不重複使用) */
    int exiting;                     /* 已開始結束流程 */
    int exit_code;
    int zombie;                      /* 所有執行緒都結束了,等待 proc_wait 來收 */
    unsigned long cpu_ticks;         /* 已經結束的執行緒用掉的 CPU 時間(還活著的另外算,見 sys_procinfo) */
    struct wait_queue waiters;       /* 正在 proc_wait 的執行緒(kernel 等特定的行程) */
    struct wait_queue child_wq;      /* 這個行程自己的執行緒,正在等「任何一個子行程結束」 */
    struct wait_queue futex[FUTEX_BUCKETS];  /* futex 的等待者,依位址分桶 */
    struct proc *next;

    struct proc_sig sig;             /* 只有 proc_signal.c */
    struct proc_mem mem;             /* 只有 proc_mem.c */
    struct proc_files files;         /* 只有 proc_fs.c */
};

/*
 * 上鎖:行程的串列與每個行程的狀態(nthreads、exiting、zombie、等待佇列)
 * 都由排程器的鎖保護(sched_lock,見 sched.h),因為「等待行程結束」「futex」
 * 需要把「檢查狀態」與「睡下去」做成不可分割的一步。
 * 會花時間或需要其他鎖的工作(配置記憶體、建立執行緒、釋放位址空間)一律放在鎖外。
 */
extern struct proc *procs;           /* 所有行程(含 zombie);定義在 proc.c */
extern int next_pid;

/* 新執行緒進入使用者模式所需的資訊,由建立者交給新執行緒 */
struct ustart {
    struct proc *p;
    uintptr_t entry, usp, arg;
};

extern struct spinlock pstate_lk;    /* 見 proc.c */

/* 各模組之間、以及給系統呼叫層呼叫的函式 */
#include "proc_calls.h"
#endif
