#ifndef WELYOS_USER_TMR_H
#define WELYOS_USER_TMR_H

/*
 * tmr.h - 三重執行加投票(Triple Modular Redundancy,TMR)
 *
 * 同一件工作交給三個各自獨立的行程去做,比較三份結果,少數服從多數:
 *   三份相同            -> 採用
 *   兩份相同、一份不同  -> 採用相同的那兩份,並回報哪一份出了錯(錯誤被「遮蔽」了)
 *   三份都不同          -> 沒有可以相信的結果,回報失敗(絕不隨便挑一份)
 *
 * 這是太空與航空系統對付「偶發的硬體錯誤」的標準做法:宇宙射線可能翻轉記憶體裡的一個位元,
 * 讓計算悄悄地得到錯的答案。單一份計算無法知道自己錯了;三份之中同時有兩份「錯成一樣」的
 * 機率則低到可以忽略。
 *
 * 這裡的三份是三個行程:各有自己的位址空間,一份的記憶體壞了不會影響另外兩份。
 * 「結果」= 寫到標準輸出的全部內容 + 結束碼,兩者都相同才算相同。
 *
 * 能對付的:某一份的記憶體被翻轉、某一份當掉、某一份卡住(逾時後被終止)。
 * 不能對付的:三份都會犯的錯(程式本身的錯誤、輸入就是錯的)、投票者自己出錯。
 *
 * 用法:tmr_run(fn, arg, 逾時毫秒, hook, hook_arg, res, &rep)
 *   fn(arg) 在每一份裡各執行一次,它的標準輸出已經接到投票者;回傳值就是結束碼。
 *   hook(不需要就傳 0)在三份都建立好、但還沒開始執行 fn 時被呼叫一次,拿得到三個 pid —
 *   測試用它來對其中一份注入故障。
 *   回傳 0 = 有結果(rep.winner 是可以採用的那一份的編號);-1 = 沒有結果。
 *   res[i].out 是 malloc 來的,用完由呼叫者 free。
 */
#include "libc.h"

#define TMR_N        3
#define TMR_OUT_MAX  8192u               /* 每一份的輸出上限;超過的視為「這一份無效」 */

struct tmr_result {
    int status;                          /* 結束碼(被終止的是 128 + 訊號編號) */
    unsigned len;                        /* 輸出的長度 */
    int invalid;                         /* 1 = 輸出超過上限或讀取失敗:不參與比較 */
    unsigned char *out;
};

struct tmr_report {
    int agree;                           /* 3、2 或 0:有幾份相同 */
    int winner;                          /* 可以採用的那一份(0~2);沒有則為 -1 */
    int odd;                             /* agree 為 2 時,不同的那一份;否則 -1 */
    int timed_out;                       /* 1 = 有一份超過時間、被終止了 */
    long pid[TMR_N];
};

typedef int (*tmr_fn)(void *arg);
typedef void (*tmr_hook)(const long pid[TMR_N], void *arg);

/* 關閉一個用不到的管線端點。失敗(例如它本來就沒開成)沒有後續可做,也不影響投票的正確性:
   正確性靠的是下面「讀到結尾」與「三份結果的比較」,不是靠這裡 */
static void tmr_close(int fd)
{
    if (fd >= 0 && sys_close(fd) != 0)
        return;
}

/* 終止一份(逾時)或計時的行程。對方已經自己結束時會失敗,那正是我們要的狀態 */
static void tmr_kill(long pid, int sig)
{
    if (pid > 0 && sys_kill(pid, sig) != 0)
        return;
}

/* 等一個子行程結束並取得結束碼。等待被訊號打斷(-E_INTR)時再試,次數有上限;
   其他的失敗(或一直被打斷)回傳 -1 — 這時最後再終止它一次、不等待地收一次,盡量不留下殭屍行程 */
static int tmr_reap(long pid, int *status)
{
    long r = -E_INTR;

    for (int tries = 0; tries < 16 && r == -E_INTR; tries++)
        r = sys_waitpid(pid, status, 0);
    if (r == pid)
        return 0;
    tmr_kill(pid, SIGKILL);
    if (sys_waitpid(pid, status, WNOHANG) == pid)
        return 0;
    return -1;
}

static int tmr_same(const struct tmr_result *a, const struct tmr_result *b)
{
    return !a->invalid && !b->invalid && a->status == b->status && a->len == b->len &&
           memcmp(a->out, b->out, a->len) == 0;
}

static int tmr_run(tmr_fn fn, void *arg, unsigned timeout_ms, tmr_hook hook, void *hook_arg,
                   struct tmr_result res[TMR_N], struct tmr_report *rep)
{
    int out[TMR_N][2], gate[2], started = 0, released = 0, rc = -1, st = 0;
    long timer = -1;
    char go = 'g';

    memset(res, 0, sizeof(res[0]) * TMR_N);
    memset(rep, 0, sizeof(*rep));
    rep->winner = rep->odd = -1;
    for (int i = 0; i < TMR_N; i++)
        out[i][0] = out[i][1] = -1;
    /* 起跑線:三份建立之後都先等在這條管線上,投票者放行才一起開始。
       這樣 hook 一定在任何一份開始計算之前執行,測試的結果才是確定的 */
    if (sys_pipe(gate) != 0)
        return -1;

    for (int i = 0; i < TMR_N; i++, started++) {
        if (!(res[i].out = malloc(TMR_OUT_MAX)) || sys_pipe(out[i]) != 0)
            break;
        long pid = sys_fork();
        if (pid < 0) {
            /* 這一份沒有建立成功:它的管線兩端都要關掉。留著寫入端的話,
               下面讀這條管線時永遠等不到結尾(寫入端在我們自己手上) */
            tmr_close(out[i][0]);
            tmr_close(out[i][1]);
            out[i][0] = out[i][1] = -1;
            break;
        }
        if (pid == 0) {
            for (int j = 0; j <= i; j++)             /* 別份的讀取端、自己的讀取端:都不是這一份該拿著的 */
                tmr_close(out[j][0]);
            tmr_close(gate[1]);
            if (sys_dup2(out[i][1], 1) != 1 || sys_close(out[i][1]) != 0 ||
                sys_read(gate[0], &go, 1) != 1)
                sys_exit(126);
            tmr_close(gate[0]);
            sys_exit(fn(arg));
        }
        rep->pid[i] = pid;
        tmr_close(out[i][1]);                 /* 寫入端只留在那一份手上:它結束時我們才讀得到結尾 */
        out[i][1] = -1;
    }
    tmr_close(gate[0]);

    if (started == TMR_N) {
        if (hook)
            hook(rep->pid, hook_arg);
        /* 計時的行程:時間到還沒結束的那幾份直接終止。投票者自己只管依序把輸出讀完,
           不必處理「讀到一半有人卡住」 */
        timer = sys_fork();
        if (timer == 0) {
            for (int i = 0; i < TMR_N; i++)
                tmr_close(out[i][0]);
            tmr_close(gate[1]);
            sys_sleep((long)timeout_ms);
            for (int i = 0; i < TMR_N; i++)
                tmr_kill(rep->pid[i], SIGKILL);
            sys_exit(0);
        }
        /* 計時的行程建立成功才放行。沒有它的話,一份卡住就會讓我們永遠讀不完:
           寧可這一次完全不執行(三份都會在起跑線上讀到結尾而結束) */
        if (timer > 0 && sys_write(gate[1], "ggg", TMR_N) == TMR_N)
            released = 1;
    }
    tmr_close(gate[1]);                       /* 沒有放行就關閉:等在起跑線上的會讀到結尾而結束 */

    for (int i = 0; i < TMR_N; i++) {
        if (out[i][0] < 0)
            continue;
        unsigned char extra[256];
        long n;
        n = 0;
        while (res[i].len < TMR_OUT_MAX &&
               (n = sys_read(out[i][0], res[i].out + res[i].len, TMR_OUT_MAX - res[i].len)) > 0)
            res[i].len += (unsigned)n;
        /* 讀取失敗要當場記下:下面第二個迴圈的讀取可能回傳 0(結尾),會把這個負值蓋掉,
           一份讀到一半就失敗的輸出就會被當成完整的 */
        if (n < 0)
            res[i].invalid = 1;
        while ((n = sys_read(out[i][0], extra, sizeof(extra))) > 0)
            res[i].invalid = 1;                      /* 超過上限:讀完丟掉,這一份不算 */
        if (n < 0)
            res[i].invalid = 1;
        tmr_close(out[i][0]);
        if (rep->pid[i] > 0 && tmr_reap(rep->pid[i], &res[i].status) != 0)
            res[i].invalid = 1;
    }
    if (timer > 0) {
        tmr_kill(timer, SIGKILL);
        if (tmr_reap(timer, &st) == 0 && st == 0)
            rep->timed_out = 1;                      /* 它是自己結束的:表示時間到了、動過手 */
    }
    if (!released)
        return -1;                                   /* 沒有執行:已建立的幾份都回收了 */

    /* 投票 */
    int ab = tmr_same(&res[0], &res[1]), ac = tmr_same(&res[0], &res[2]), bc = tmr_same(&res[1], &res[2]);
    if (ab && ac) {
        rep->agree = 3;
        rep->winner = 0;
    } else if (ab || ac || bc) {
        rep->agree = 2;
        rep->winner = ab || ac ? 0 : 1;
        rep->odd = ab ? 2 : ac ? 1 : 0;
    }
    if (rep->winner >= 0)
        rc = 0;
    return rc;
}

#endif
