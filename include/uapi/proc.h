#ifndef WELYOS_UAPI_PROC_H
#define WELYOS_UAPI_PROC_H
/*
 * uapi/proc.h - 行程相關:等待的選項、行程與系統的狀態、訊號編號、特權、即時工作的統計
 *
 * 這個目錄(uapi = user API)放的是 kernel 與使用者程式「共用」的定義:系統呼叫的編號、
 * 經由系統呼叫傳遞的結構、兩邊都要認得的常數。兩邊引入的是同一個檔案,
 * 所以不會再有「kernel 改了、使用者端忘了跟著改」的問題。
 * 規則:這裡的檔案只能引入 <stdint.h> / <stddef.h> 與同目錄的檔案,不能依賴 kernel 內部的任何東西;
 * 結構一律用固定寬度的型別,三個架構(32 與 64 位元)的佈局才會相同。
 * 已經公開的編號與欄位只增不改。
 */
#include <stdint.h>

/* SYS_waitpid 的選項 */
#define WNOHANG   1         /* 不等待:還沒有子行程結束就立刻回傳 0 */
#define WUNTRACED 2         /* 子行程「被暫停」也回報(它不會被回收) */

/* 被暫停時寫入 *status 的值:WSTOPPED_BASE | 訊號編號。一般的結束碼不會落在這一段 */
#define WSTOPPED_BASE  0x80000000u
#define WIFSTOPPED(s)  (((unsigned)(s) & 0xFFFFFF00u) == WSTOPPED_BASE)
#define WSTOPSIG(s)    ((int)((unsigned)(s) & 0xFFu))

/* SYS_sysinfo 回報的內容 */
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

/* SYS_scrub 回報的內容 */
struct scrubinfo {
    uint32_t checksums;         /* 0 = 這個磁碟(或沒有磁碟)沒有檢查碼,無法巡檢 */
    uint32_t blocks;            /* 檔案系統的區塊總數 */
    uint32_t bad;               /* 這一遍發現幾個損壞的區塊 */
    uint32_t read_errors;       /* 開機至今,讀取時因檢查碼不符而拒絕的次數 */
    uint32_t sb_restored;       /* 這次開機時 superblock 是從備份救回來的 */
    uint32_t pad;
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

/* SYS_procinfo 回報的內容 */
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
/* 訊號編號(沿用 Unix 的定義)。終止:INT、KILL、TERM(被終止的行程結束碼為 128 + 編號);
   暫停:STOP、TSTP(Ctrl-Z)、TTIN(背景行程想讀鍵盤);繼續:CONT */
#define SIGINT  2
#define SIGKILL 9
#define SIGUSR1 10
#define SIGUSR2 12
#define SIGTERM 15
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21

/* 特權(SYS_capget / SYS_capdrop;說明見 kernel 的 cred.h)。只能減少,不能增加 */
#define CAP_FS     1u            /* 不受檔案權限限制;可以更改任何檔案的擁有者與權限 */
#define CAP_KILL   2u            /* 可以對別人的行程送訊號 */
#define CAP_SETID  4u            /* 可以把自己的 uid / gid 改成任何值 */
#define CAP_ADMIN  8u            /* 系統管理(保留給之後的功能,例如設定網路) */
#define CAP_ALL    15u

/* SYS_rt_wait 回報的內容:週期性即時工作的統計(說明見 kernel 的 sched.h) */
struct rt_stats {
    uint32_t period_ms, budget_ms;
    uint32_t releases;       /* 準時開始了幾個週期 */
    uint32_t misses;         /* 錯過了幾次期限 */
    uint32_t overruns;       /* 超出預算幾次 */
    uint32_t max_used_ms;    /* 單一週期用掉最多的 CPU 時間 */
    uint32_t throttled;      /* 1 = 目前因超出預算而被降級中 */
    uint32_t total_util;     /* 全系統已接受的即時工作合計佔 CPU 的千分比 */
};

/* 佈局的檢查:這些結構在 32 與 64 位元的架構上必須一樣大(欄位都對齊在自然的位置上) */
_Static_assert(sizeof(struct sysinfo) == 176, "ABI: struct sysinfo");
_Static_assert(sizeof(struct scrubinfo) == 24, "ABI: struct scrubinfo");
_Static_assert(sizeof(struct timeval) == 16, "ABI: struct timeval");
_Static_assert(sizeof(struct procinfo) == 64, "ABI: struct procinfo");
_Static_assert(sizeof(struct rt_stats) == 32, "ABI: struct rt_stats");
#endif
