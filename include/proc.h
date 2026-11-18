#ifndef WELYOS_PROC_H
#define WELYOS_PROC_H
#include <stdint.h>

/*
 * 行程(process)= 一個獨立的位址空間 + 在裡面執行的一條或多條執行緒。
 *
 * 行程在「使用者模式」執行:CPU 處於低權限,不能執行特權指令、不能存取
 * kernel 的記憶體,也看不到其他行程的記憶體。它要做任何需要權限的事
 * (輸出、睡眠、建立執行緒…)都只能透過「系統呼叫」請 kernel 代勞。
 * 行程做錯事(存取不該碰的位址、執行特權指令)只會讓它自己被終止,
 * kernel 與其他行程不受影響。
 */

/* 建立行程,執行檔案系統中 path 指到的程式(例如 "/bin/init")。
   argv 是傳給程式 main 的參數:以 NULL 結尾的字串陣列;傳 NULL 表示只有程式名稱。
   回傳行程編號(pid);失敗(檔案不存在、不是合法的程式、記憶體不足)回傳 -1 */
int proc_spawn(const char *path, const char *const argv[]);

/* 等待行程結束並取得它的結束碼,同時釋放行程的紀錄。pid 不存在回傳 -1。
   因例外被終止的行程,結束碼為 PROC_KILLED_FAULT;被 proc_kill 終止的為指定的值 */
int proc_wait(int pid);

/* 強制終止行程(它的所有執行緒會在下次進入 kernel 時結束)。pid 不存在回傳 -1 */
int proc_kill(int pid, int code);

#define PROC_KILLED_FAULT (-1)

/* 對行程群組 pgid 的所有行程送出訊號 sig(終止它們)。回傳送到的行程數。
   呼叫時必須持有排程器的鎖;主控台收到 Ctrl-C 時使用 */
int proc_signal_group_locked(int pgid, int sig);

/* 主控台(console.c)判斷「誰可以讀鍵盤」時使用;呼叫時都必須持有排程器的鎖 */
int proc_self_pgid(void);                /* 目前執行緒所屬行程的群組;kernel 執行緒為 0 */
int proc_self_stopped(void);             /* 目前執行緒所屬的行程是否被暫停了 */
int proc_group_alive_locked(int pgid);   /* 群組裡還有沒有活著的行程 */

/* ---- 以下由各架構的例外處理程式呼叫 ---- */

/* 系統呼叫的進入點:num 為呼叫編號,a0~a2 為參數;回傳值會放回使用者的暫存器。
   tf 指向這次進入 kernel 時存下來的暫存器(fork 要複製它給子行程) */
long syscall_dispatch(long num, long a0, long a1, long a2, void *tf);

/* 使用者模式發生 page fault 時先呼叫:addr 為出錯的位址,write 表示是不是寫入。
   回傳 0 表示已經處理好(寫入時複製、stack 成長),讓程式重新執行那條指令即可;
   回傳 -1 表示是真正的錯誤,呼叫者應以 user_fault 終止行程 */
int proc_page_fault(uintptr_t addr, int write);

/* 使用者模式發生無法處理的例外:印出原因並終止該行程。不會返回 */
void user_fault(const char *what, uintptr_t pc, uintptr_t addr) __attribute__((noreturn));

/* 每次要從 kernel 返回使用者模式之前呼叫:若這條執行緒已被要求終止,就在這裡結束;
   行程被暫停就在這裡等;有訊號要交給處理函式,就把 tf(進入 kernel 時存下來的暫存器)
   改成去執行那個函式 */
void user_return_check(void *tf);

/* 系統呼叫編號(使用者端的對應定義在 user/usys.h,兩邊必須一致) */
enum {
    SYS_write = 1,          /* (fd, buf, len)  寫入檔案描述符;回傳寫入的長度。1 = 標準輸出 */
    SYS_exit,               /* (code)          結束整個行程 */
    SYS_getpid,             /* ()              行程編號 */
    SYS_yield,              /* ()              讓出 CPU */
    SYS_sleep,              /* (ms)            睡眠 */
    SYS_thread_create,      /* (entry, arg)    在同一個行程內建立執行緒;回傳 tid */
    SYS_thread_exit,        /* ()              結束目前的執行緒 */
    SYS_gettid,             /* ()              執行緒編號 */
    SYS_futex_wait,         /* (addr, val)     若 *addr == val 就睡到被喚醒;否則立刻返回 1 */
    SYS_futex_wake,         /* (addr)          喚醒所有睡在 addr 上的執行緒;回傳喚醒的數量 */
    /* ---- 檔案(失敗時回傳負的錯誤碼,見 include/fs.h 的 E_*) ---- */
    SYS_open,               /* (path, flags)   開啟檔案;回傳檔案描述符 */
    SYS_close,              /* (fd) */
    SYS_read,               /* (fd, buf, len)  讀取;回傳讀到的長度,0 = 檔案結尾 */
    SYS_readdir,            /* (fd, dirent)    讀取目錄的下一筆;0 = 取得,1 = 沒有了 */
    SYS_mkdir,              /* (path) */
    SYS_unlink,             /* (path)          刪除檔案或空的目錄 */
    SYS_stat,               /* (path, stat)    查詢類型與大小 */
    /* ---- 行程 ---- */
    SYS_spawn,              /* (path, argv)    建立新行程執行 path;回傳 pid。新行程繼承 0、1、2 號描述符 */
    SYS_exec,               /* (path, argv)    目前的行程改為執行 path;成功則不返回 */
    SYS_wait,               /* (pid)           等待自己的某個子行程結束,回傳它的結束碼 */
    SYS_dup2,               /* (oldfd, newfd)  讓 newfd 也指向 oldfd 的那次開啟;回傳 newfd */
    SYS_fork,               /* ()              複製目前的行程;父行程得到子行程的 pid,子行程得到 0 */
    SYS_pipe,               /* (fds)           建立管線;fds[0] 為讀端、fds[1] 為寫端的描述符 */
    SYS_waitpid,            /* (pid, status, options)  等子行程結束(pid = -1:任何一個);回傳它的 pid,
                                               結束碼寫入 *status。options 可為 WNOHANG、WUNTRACED */
    SYS_kill,               /* (pid, sig)      送訊號(終止 2、9、15;暫停 19、20、21;繼續 18);
                                               pid < 0 表示整個群組 -pid */
    SYS_setpgid,            /* (pid, pgid)     把自己(pid 0)或子行程放進行程群組;pgid 0 = 以它的 pid 為群組 */
    SYS_tcsetfg,            /* (pgid)          設定主控台的前景群組:Ctrl-C 終止、Ctrl-Z 暫停這一組,
                                               也只有這一組可以讀鍵盤。0 = 沒有(輪到呼叫者自己讀) */
    SYS_procinfo,           /* (after, info)   查詢編號大於 after 的下一個行程;0 = 取得,1 = 沒有更多了 */
    SYS_sync,               /* ()              把檔案系統還留在記憶體裡的修改寫到磁碟上 */
    SYS_statfs,             /* (path, info)    path 所在的檔案系統的用量(struct fsinfo,見 fs.h) */
    SYS_sysinfo,            /* (info)          整個系統的狀態:運作時間、各 CPU 的使用量、記憶體(struct sysinfo) */
    SYS_tcsetmode,          /* (mode)          設定主控台的輸入模式(CON_RAW、CON_NOECHO;0 = 一般) */
    SYS_lseek,              /* (fd, off, whence)  改變讀寫位置(SEEK_SET / SEEK_CUR / SEEK_END);回傳新的位置 */
    SYS_rename,             /* (old, new)      改名或搬移(同一個檔案系統內;目的已存在則失敗) */
    SYS_link,               /* (old, new)      硬連結:讓 new 成為檔案 old 的另一個名稱 */
    SYS_chdir,              /* (path)          改變目前的工作目錄(相對路徑的起點) */
    SYS_getcwd,             /* (buf, len)      取得目前的工作目錄(以 0 結尾的字串);回傳長度 */
    SYS_sbrk,               /* (incr)          把 heap 的結尾移動 incr 個位元組;回傳移動之前的結尾 */
    SYS_signal,             /* (sig, handler, restorer)  設定訊號的處理方式;回傳原本的。handler 為
                                               SIG_DFL(0)、SIG_IGN(1)或函式的位址 */
    SYS_sigreturn,          /* (frame)         處理函式結束時由 restorer 呼叫:還原被打斷時的暫存器 */
    SYS_time,               /* (tv)            現在的日期時間(struct timeval:1970-01-01 UTC 至今的秒與微秒) */
    /* ---- 身分與權限(見 cred.h、fs.h 的「權限」) ---- */
    SYS_getuid,             /* ()              自己的使用者編號 */
    SYS_getgid,             /* ()              自己的群組編號 */
    SYS_setuid,             /* (uid, gid)      改變自己的身分(需要 CAP_SETID);改成非 root 會失去所有特權 */
    SYS_capget,             /* ()              自己目前的特權(CAP_* 的組合) */
    SYS_capdrop,            /* (mask)          永久放棄 mask 裡的特權;回傳剩下的 */
    SYS_chmod,              /* (path, mode)    更改權限位元(擁有者或 CAP_FS) */
    SYS_chown,              /* (path, uid, gid) 更改擁有者與群組(CAP_FS) */
    /* ---- 記憶體 ---- */
    SYS_mmap,               /* (len)           要一塊內容為 0 的記憶體(匿名對映);回傳位址 */
    SYS_munmap,             /* (addr)          歸還 mmap 得到的一整塊 */
    SYS_memlimit,           /* (kb)            記憶體配額:0 = 查詢;調低隨時可以,調高需要 CAP_ADMIN。回傳配額(KB) */
    /* ---- 網路(見 netif.h;位址與埠號是主機順序的整數) ---- */
    SYS_ping,               /* (ip, timeout_ms)  送出 echo request 並等回答;回傳來回的毫秒數 */
    SYS_udp_open,           /* (port)          建立綁定在 port 的 UDP socket(0 = 自動挑選);回傳檔案描述符 */
    SYS_udp_send,           /* (fd, msg)       送出一個封包(struct udpmsg:目的位址、埠、資料);回傳長度 */
    SYS_udp_recv,           /* (fd, msg, timeout_ms)  等一個封包;填入來源與資料,回傳長度 */
    SYS_netinfo,            /* (info)          網路的設定與統計(struct netinfo,見 netif.h) */
    /* ---- 黑盒子(見 blackbox.h) ---- */
    SYS_bbread,             /* (index, rec)    讀取一筆事件紀錄(0 = 最新的);0 = 取得,1 = 沒有這麼多筆 */
    SYS_bblog,              /* (type, text)    留下一筆紀錄(BB_NOTE 或 BB_RESTART);需要 CAP_ADMIN */
};

/* SYS_waitpid 的選項 */
#define WNOHANG   1         /* 不等待:還沒有子行程結束就立刻回傳 0 */
#define WUNTRACED 2         /* 子行程「被暫停」也回報(它不會被回收) */

/* 被暫停時寫入 *status 的值:WSTOPPED_BASE | 訊號編號。一般的結束碼不會落在這一段 */
#define WSTOPPED_BASE  0x80000000u
#define WIFSTOPPED(s)  (((unsigned)(s) & 0xFFFFFF00u) == WSTOPPED_BASE)
#define WSTOPSIG(s)    ((int)((unsigned)(s) & 0xFFu))

/* SYS_sysinfo 回報的內容(使用者端的定義在 user/usys.h,必須一致) */
#define SYSINFO_MAX_CPUS 8
struct sysinfo {
    uint64_t uptime_ticks;      /* 開機至今的 tick 數 */
    uint32_t hz;                /* 每秒幾個 tick */
    uint32_t ncpu;              /* 參與排程的 CPU 數 */
    uint32_t nprocs, nthreads;
    uint64_t mem_total_kb, mem_free_kb;
    uint64_t ctx_switches;      /* 累計的執行緒切換次數 */
    struct {
        uint64_t busy, idle;    /* 這顆 CPU 忙碌 / 閒置的 tick 數(累計) */
    } cpu[SYSINFO_MAX_CPUS];
};

/* SYS_udp_send / SYS_udp_recv 的參數(使用者端的定義在 user/usys.h,必須一致)。
   送出:ip、port 是目的,data / len 是要送的資料。
   接收:data / len 是緩衝區與它的大小;返回時 ip、port 是來源,len 是實際的長度 */
struct udpmsg {
    uint32_t ip;
    uint16_t port, pad;
    uint32_t len, pad2;
    uint64_t data;              /* 使用者位址(固定用 64 位元,三個架構的格式才相同) */
};

/* SYS_time 回報的內容 */
struct timeval {
    uint64_t sec;               /* 1970-01-01 00:00:00 UTC 至今的秒數 */
    uint32_t usec;              /* 秒以下的部分(微秒) */
    uint32_t pad;
};

/* SYS_tcsetmode 的模式(可以相加) */
#define CON_RAW    1            /* 不等整行:每個按鍵立刻交給讀取者,不做行編輯 */
#define CON_NOECHO 2            /* 不把打的字顯示出來 */

/* SYS_procinfo 回報的內容(使用者端的定義在 user/usys.h,必須一致) */
enum { PROC_RUNNING = 1, PROC_SLEEPING, PROC_ZOMBIE, PROC_STOPPED };
struct procinfo {
    int pid, ppid, pgid;
    int state;              /* PROC_RUNNING:有執行緒正在執行或等著執行;PROC_SLEEPING:全部在等待;
                               PROC_ZOMBIE:已結束,等父行程來收;PROC_STOPPED:被暫停了 */
    int nthreads;
    unsigned int cpu_ticks; /* 累計用掉的 CPU 時間(tick,1 tick = 1ms) */
    unsigned int mem_kb;     /* 對映的記憶體(KB;含和其他行程共用的程式碼與尚未複製的頁) */
    unsigned int uid;        /* 行程的使用者編號 */
    char name[32];
};
#endif
