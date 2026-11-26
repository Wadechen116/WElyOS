#ifndef WELYOS_LOCKHOLD_H
#define WELYOS_LOCKHOLD_H
/*
 * lockhold:sched_lk / time_lk 持有區間量測的 kernel backend(I-349 lock-hold,055;契約 Codex 079)。
 *
 * 量測邏輯全在 include/lockhold_model.h(純模型,Codex 擁有);本模組只負責:
 *   - 每顆 CPU 一份模型,只由該 CPU 在關中斷下寫(hook 與 safepoint),遠端從不直接改模型;
 *   - 控制請求(START / STOP)以 seqlock 發布,各 CPU 只在 lockhold_safepoint 套用並寫 ACK;
 *   - STOP 成功後以 seqlock 把模型發布成 per-CPU 副本,lockhold_snapshot 只讀副本。
 * 單位是 arch_lh_stamp 的原始計數,不換算。lockhold 不知道 spinlock 的內部,也不知道哪把是 sched / time:
 * 呼叫端以 LH_SCHED / LH_TIME(1..2)標明。
 */
#include <stdint.h>
#include "cpu.h"
#include "lockhold_model.h"

/* ---- spinlock hook(呼叫時中斷已關;零配置、零輸出、不取任何鎖)---- */
void lockhold_acquired(uint8_t id);    /* 已成功取得 id 那把鎖之後 */
void lockhold_releasing(uint8_t id);   /* 即將放開 id 那把鎖之前 */

/* 每顆 CPU 計時器中斷入口、尚未取 time_lk / sched_lk 時呼叫(中斷已關)。唯一套用控制請求的地方。 */
void lockhold_safepoint(void);

/* ---- 控制端(執行緒情境、中斷開著、不持有任何 spinlock;同時只允許一個控制端)---- */
enum lh_ack {
    LH_ACK_NONE = 0,         /* 尚未 ACK(只在內部使用) */
    LH_ACK_OK,               /* 已在安全點套用成功(START:lh_begin 成功;STOP:lh_stop 成功且 held == 0) */
    LH_ACK_FAIL,             /* 已套用但模型拒絕(session 重用、來源不合格、STOP 時 phase 為 OFF 等) */
    LH_ACK_INCONSISTENT,     /* 安全點時本 CPU 仍持有目標鎖(held != 0)或 session 不符 */
    LH_ACK_TIMEOUT,          /* 逾時內沒有 ACK(不強制釋鎖) */
    LH_ACK_OFFLINE,          /* 目標 CPU 不在線上 */
    LH_ACK_BUSY,             /* 另一個控制端正在進行,本次未發布 */
};
#define LH_SNAP_TRIES 1000

/*
 * 發布 START(session, 目標 mask),等目標 CPU 在安全點 ACK,最多 timeout_ticks 個 timer tick
 * (另有迴圈次數上限,timer 不前進時也必定返回)。ack[i] 為每顆 CPU 的結果(不在 mask 內為 LH_ACK_NONE)。
 * session 必須非零且大於之前任何一次 START 的 session(單調遞增、不 wrap、不重用);否則全部 LH_ACK_FAIL、
 * 不發布。回傳 1 = mask 內全部 LH_ACK_OK。逾時的 CPU 之後仍可能套用,但它的結果不會被當成完整。
 */
int lockhold_start(uint32_t session, unsigned mask, uint32_t timeout_ticks, uint8_t ack[MAX_CPUS]);
/* 對最近一次 START 的 session 發布 STOP,等目標 ACK FROZEN。語義同上。 */
int lockhold_stop(unsigned mask, uint32_t timeout_ticks, uint8_t ack[MAX_CPUS]);

/* panic 時呼叫(任一 CPU、任何情境):只設全域旗標,不碰遠端模型。各 CPU 下一次 hook / 安全點自己
   lh_fail(LH_PANIC);snapshot 另把 panic 狀態帶出,讀者不必等目標 CPU 再執行。 */
void lockhold_panic(void);

/* 一顆 CPU 已發布副本的一致快照 */
struct lockhold_snap {
    int ok;                  /* 1 = LH_SNAP_TRIES 內讀到一致的一份;0 = BUSY(內容不可用) */
    uint32_t pub_no;         /* 發布次數(0 = 從未發布) */
    uint32_t panic;          /* 讀取時全域 panic 旗標 */
    struct lh_model m;       /* 已發布的模型;panic 時 m.flags 另 OR LH_PANIC */
};
/* 只供單一控制端(測試執行緒)呼叫:內部有每顆 CPU 一份的靜態暫存,避免 2 KB 放 stack。 */
void lockhold_snapshot(int cpu, struct lockhold_snap *out);

static inline const char *lockhold_ack_name(uint8_t a)
{
    switch (a) {
    case LH_ACK_NONE: return "none";
    case LH_ACK_OK: return "ok";
    case LH_ACK_FAIL: return "fail";
    case LH_ACK_INCONSISTENT: return "inconsistent";
    case LH_ACK_TIMEOUT: return "timeout";
    case LH_ACK_OFFLINE: return "offline";
    case LH_ACK_BUSY: return "busy";
    }
    return "?";
}

#endif
