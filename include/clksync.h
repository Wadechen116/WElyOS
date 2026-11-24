#ifndef WELYOS_CLKSYNC_H
#define WELYOS_CLKSYNC_H
#include <stdint.h>
#include "cpu.h"

/*
 * 跨 CPU 共用計數器的同步驗證(I-337;Codex 審查 209/210;規則來自
 * tests/host/proc/probe_i337_counter_sync.c)。
 *
 * I-337 讓任何一顆 CPU 依共用計數器(i386 TSC、RISC-V time、AArch64 cntpct_el0)推進系統時間,
 * 前提是各 CPU 讀到的計數器一致。這個前提由這裡在開機時「量測」,而不是假設:
 *   第 0 顆(參考)讀 ref_before → AP 讀 remote → 第 0 顆讀 ref_after,三者以無鎖的旗標交接。
 *   AP 的偏移量必落在 [remote - ref_after, remote - ref_before]。
 * 每顆 AP 取 CLKSYNC_SAMPLES 次;每次三個值都必須嚴格遞增(各 CPU 單調);寬度超過容許值的
 * 取樣不採用(無法判斷),太多時整輪重試;採用的取樣偏移區間都必須落在 ±tol 之內。
 * tol = 四分之一個 tick(timer_tick_counts() >> 2,不用 64 位元除法)。
 *
 * 不合格時(I-341):依序換到下一層備援的共用計數器(timer_clock_fallback;i386 TSC → HPET → PM timer),
 * 對每個新來源做同樣的完整驗證;合格就繼續由任何 CPU 推進(狀態 OK、fallback = 1)。
 * 沒有備援、或備援也不合格時才失敗即關閉(fail closed):停用共用路徑(timer_shared_disable),
 * 退回只由第 0 顆推進時間(降級,仍是單點)。每一步都記在黑盒子與開機訊息裡。
 * riscv64 / aarch64 的計數器在架構上就是全系統共用的單一時基,沒有備援(見 arch.h)。
 */

enum clksync_state {
    CLKSYNC_UNCHECKED = 0,   /* 還沒驗證 */
    CLKSYNC_SINGLE,          /* 只有一顆 CPU:不需要跨 CPU 一致 */
    CLKSYNC_OK,              /* 所有 AP 合格,共用路徑啟用 */
    CLKSYNC_DEGRADED,        /* 驗證失敗或沒有共用計數器:只由第 0 顆推進(降級) */
};

/* 不合格的原因(每顆 CPU 一個,也用於整體) */
enum clksync_reason {
    CLKSYNC_R_NONE = 0,
    CLKSYNC_R_NO_COUNTER,    /* timer_clock_hz() == 0 或沒有每 tick 計數 */
    CLKSYNC_R_NO_RESPONSE,   /* AP 沒有在時限內回應交接 */
    CLKSYNC_R_NONMONOTONIC,  /* 某顆 CPU 的取樣沒有嚴格遞增 */
    CLKSYNC_R_WIDE,          /* 重試後可用的取樣仍太少(區間都太寬,無法判斷) */
    CLKSYNC_R_OFFSET,        /* 偏移區間超出 ±tol(固定偏移或漂移) */
    CLKSYNC_R_BUSY,          /* 另一次驗證正在進行(只會出現在測試的乾跑) */
    CLKSYNC_R_FORCED,        /* 測試:開機參數 clock=fallback / cpu0only / pmtimer 強制判定不合格 */
    CLKSYNC_R_DRIFT,         /* 兩輪之間偏移變化超過 dmax,或重試後仍無法判斷(I-345) */
};

/* 測試用的強制降級(只在 selftest 時由 kmain 設定,見 clksync_force) */
enum clksync_force_mode {
    CLKSYNC_FORCE_NONE = 0,
    CLKSYNC_FORCE_FALLBACK,  /* 開機驗證強制不合格 → 走「換到備援計數器並驗證」 */
    CLKSYNC_FORCE_CPU0ONLY,  /* 開機驗證強制不合格,而且不用備援 → 走「只由第 0 顆推進」的降級 */
    CLKSYNC_FORCE_PMTIMER,   /* 名稱不是 pmtimer 的來源一律強制不合格(含備援的驗證)→ 走完整條備援鏈 */
    CLKSYNC_FORCE_DRIFT,     /* 第一次再驗證時,讓參考以外的每顆 CPU 回報的計數加 3/16 tick(單輪仍合格) */
};

#define CLKSYNC_SAMPLES  16      /* 每顆 AP 每輪的取樣數 */
#define CLKSYNC_ATTEMPTS 4       /* 可用取樣不足時,整輪最多重試幾次 */

struct clksync_cpu {
    int      reason;             /* enum clksync_reason */
    int64_t  min_low, max_high;  /* 採用的取樣中,偏移區間的最小下界與最大上界(計數) */
    uint64_t max_abs;            /* max(-min_low, max_high, 0):最大可能偏移(計數) */
    uint64_t tightest;           /* 最窄的一次括號寬度 ref_after - ref_before(計數) */
    unsigned used, wide, attempts;
    /* 跨輪比較(I-345;只有再驗證的結果有):相對第 0 顆的偏移,本輪減基準輪的變化區間與分類 */
    int64_t  d_lo, d_hi;
    int      dclass;             /* enum clksync_cross */
};

/* 跨輪比較的分類(I-345,Codex 006 的三態模型) */
enum clksync_cross {
    CLKSYNC_X_NONE = 0,      /* 沒有比較(不是再驗證、或基準輪來源不同) */
    CLKSYNC_X_ACCEPT,        /* 變化區間整個落在 [−dmax, dmax] */
    CLKSYNC_X_UNCERTAIN,     /* 橫跨邊界:無法證明(只有這種會重跑第二輪) */
    CLKSYNC_X_REJECT,        /* 整個落在外面:確定漂移 */
};

struct clksync_result {
    int      ok;                 /* 1 = 所有 AP 合格 */
    int      reason;             /* 第一個失敗的原因 */
    int      bad_cpu;            /* 第一個失敗的 CPU;-1 = 沒有 */
    int      ncpu;
    uint64_t tol;                /* 容許偏移(計數) */
    uint64_t iv;                 /* 每 tick 計數 */
    uint64_t at;                 /* 驗證開始時的計數值 */
    int      ref;                /* 參考 CPU(執行驗證的那一顆;開機驗證時是第 0 顆) */
    const char *src;             /* 驗證的是哪一個計數器(timer_clock_name) */
    int      cross;              /* 跨輪比較的整體分類(enum clksync_cross) */
    unsigned cross_tries;        /* 第二輪跑了幾次(無法判斷才重跑) */
    uint64_t dmax;               /* 允許的偏移變化(計數)= 每 tick 計數 >> 3 */
    struct clksync_cpu cpu[MAX_CPUS];   /* 以 CPU 編號索引;參考 CPU 那一格不填 */
};

/* smp_init 在所有 AP 回報後、smp_sched_start 之前呼叫(第 0 顆、開著中斷)。失敗時降級 */
void clksync_boot_check(int ncpu);
/* 第一次驗證之後至少 1 秒再驗證一次,抓漂移:本輪必須合格,而且和基準輪(同一個來源)比較,每顆 CPU
   相對第 0 顆的偏移變化不超過 dmax(I-345)。再驗證時換了來源的話,新來源 1 秒後再做一輪(每個來源
   最多一次,整條備援鏈有上限)。之後呼叫不做事。可能睡到滿 1 秒 */
void clksync_recheck(void);
/* 由 smp_poll 在收到 IPI_CLKSYNC 時呼叫(中斷關閉中) */
void clksync_remote(void);

/* 查詢(測試與診斷用) */
int         clksync_state(void);                  /* enum clksync_state */
const char *clksync_reason_name(int reason);
/* 0 = 開機驗證、1 = 1 秒後的再驗證、2 = 換到備援計數器之後的驗證;未做為 NULL */
const struct clksync_result *clksync_last(int which);
int         clksync_fallback_used(void);          /* 1 = 已換到備援計數器(而且驗證合格) */
/* 測試:kmain 在 smp_init 之前呼叫(只在 selftest 時),影響開機驗證;enum clksync_force_mode */
void        clksync_force(int mode);
int         clksync_force_mode(void);
/* 測試(clock=drift):被注入的那一次再驗證的結果(之後的再驗證會覆蓋 clksync_last(1));沒有為 NULL */
const struct clksync_result *clksync_drift_first(void);
uint32_t    clksync_milli_ticks(uint64_t counts, uint64_t iv);  /* 計數換成千分之一 tick(上限 4000) */

/*
 * 測試專用(只有 selftest 會呼叫):
 *   clksync_run_dry()       執行一次完整驗證,但不論結果都不改變系統時間的模式
 *   clksync_test_inject()   讓第 cpu 顆 AP 回報的計數加上 offset、或讀完後再空轉 delay 個計數才回應
 *                           (把括號撐寬)。cpu < 0 清除。用來證明拒絕路徑真的會被走到(負對照)
 */
int  clksync_run_dry(struct clksync_result *out);
void clksync_test_inject(int cpu, uint64_t offset, uint64_t delay);
#endif
