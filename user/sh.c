/*
 * sh.c - 簡單的命令直譯器(shell)
 *
 * 做的事情就是一個迴圈:讀一行 -> 切成一個個字 -> 第一個字是程式名稱,
 * 其餘是參數 -> 建立行程執行它 -> 等它結束 -> 再讀下一行。
 *
 * 用法:
 *   sh              互動模式:從標準輸入讀命令,每次先印出提示符號 "$ "
 *   sh 檔案         依序執行檔案裡的命令(script),不印提示符號
 *
 * 支援:
 *   命令 參數...            名稱不含 "/" 時到 /bin 底下找
 *   命令 > 檔案             輸出寫入檔案(檔案原本的內容清掉)
 *   命令 >> 檔案            輸出接在檔案最後
 *   命令 < 檔案             輸入來自檔案
 *   命令1 | 命令2 | ...     管線:前一個命令的輸出成為後一個命令的輸入
 *   命令 &                  背景執行:不等它結束,立刻回到提示符號(見下方「背景工作」)
 *   "含 空白 的 參數"       雙引號內的空白不拆開
 *   NAME=value / $NAME / ${NAME} / $?  shell-local 變數與上一個結束碼
 *   * / ?                   未被引號保護的路徑萬用字元
 *   # 註解                  這一行從 # 起到結尾都忽略
 * 內建命令(由 shell 自己處理,不建立行程):
 *   exit [數字]             結束 shell;沒有給數字時以上一個命令的結束碼結束
 *   cd [目錄]               改變目前的工作目錄(沒有給:回到 /)。必須是內建的:
 *                           工作目錄是每個行程自己的,另外啟動一個程式去改,改到的是那個程式的
 *   pwd                     印出目前的工作目錄
 *   limit [KB]              顯示或調低記憶體配額;之後啟動的命令都受它限制(只能調低)
 *   status                  印出上一個命令的結束碼
 *   help                    列出可用的命令
 *   jobs                    列出背景工作
 *   fg [編號]               把背景(或暫停中)的工作帶回前景並等它結束(沒有給編號:最後建立的那個)
 *   bg [編號]               讓暫停中的工作在背景繼續執行(沒有給編號:最後暫停的那個)
 *   wait                    等所有執行中的背景工作結束
 * 按鍵(由 kernel 的主控台處理,見 kernel/console.c):
 *   Ctrl-C                  終止前景的命令
 *   Ctrl-Z                  暫停前景的命令,回到提示符號;之後用 fg 或 bg 讓它繼續
 *
 * 互動行編輯:16 筆歷史(Up/Down)、左右/Home/End/Delete、Tab 路徑補全;
 * 以位元組編輯,不是 Unicode grapheme 編輯器。變數不經由環境繼承給子行程。
 * shell 的結束碼:exit 指定的值,或(輸入結束時)最後一個命令的結束碼。
 */
#include "ulib.h"

#define MAX_ARGS   15
#define MAX_STAGES 6                             /* 一條管線最多由幾個命令組成 */
#define SAVE_IN    14                            /* 重新導向時暫存原本標準輸入 / 輸出的描述符編號 */
#define SAVE_OUT   15
#define SAVE_SYNC  13                            /* 「> 檔案」的那個檔案:命令結束後由 shell 同步並確認寫入成功 */

/* 從檔案描述符一次讀一塊,再一行一行交出去。
   直接一次讀一個位元組也可以,但每個位元組都要一次系統呼叫,慢很多 */
struct reader {
    long fd;
    char buf[256];
    long pos, len;
};

/* 讀一行到 out(不含換行,以 0 結尾)。回傳 1 = 讀到一行,0 = 輸入結束 */
static int read_line(struct reader *r, char *out, unsigned cap)
{
    unsigned n = 0;
    int overflow = 0;

    for (;;) {
        if (r->pos >= r->len) {
            r->len = sys_read(r->fd, r->buf, sizeof(r->buf));
            r->pos = 0;
            if (r->len <= 0) {                   /* 輸入結束(或出錯) */
                out[n] = 0;
                return overflow ? -1 : n > 0;     /* 過長整行拒絕,不執行截斷的前綴 */
            }
        }
        char c = r->buf[r->pos++];
        if (c == '\n') {
            out[n] = 0;
            return overflow ? -1 : 1;
        }
        if (n < cap - 1)
            out[n++] = c;
        else
            overflow = 1;
    }
}

#include "sh_words.h"
#include "sh_edit.h"

static void print_num(const char *before, long v, const char *after)
{
    struct line l = { .len = 0 };

    line_str(&l, before);
    line_num(&l, v);
    line_str(&l, after);
    line_out(&l);
}

/* 一條管線中的一個命令 */
struct stage {
    char *argv[MAX_ARGS + 1];
    int argc;
    const char *in_path;                         /* "< 檔案";沒有則為 0 */
    const char *out_path;                        /* "> 檔案" 或 ">> 檔案" */
    int append;
};

/* 把切好的字分配到各個命令:遇到 "|" 換下一個命令,遇到重新導向記下檔名。
   回傳命令的個數;語法錯誤回傳 -1 */
static int parse(char **words, int n, struct stage *st)
{
    int ns = 1;

    st[0] = (struct stage){ .argc = 0 };
    for (int i = 0; i < n; i++) {
        struct stage *cur = &st[ns - 1];
        int out = sh_operator(words[i], ">"), app = sh_operator(words[i], ">>"), in = sh_operator(words[i], "<");

        if (sh_operator(words[i], "|")) {
            if (cur->argc == 0 || ns >= MAX_STAGES || i + 1 >= n)
                return -1;                       /* "|" 的前後都要有命令 */
            st[ns++] = (struct stage){ .argc = 0 };
        } else if (out || app || in) {
            if (i + 1 >= n)
                return -1;                       /* 後面要接檔名 */
            if (in) {
                if (sh_operator(words[i+1], "|") || sh_operator(words[i+1], "&") ||
                    sh_operator(words[i+1], ">") || sh_operator(words[i+1], ">>") || sh_operator(words[i+1], "<")) return -1;
                cur->in_path = words[++i];
            } else {
                if (sh_operator(words[i+1], "|") || sh_operator(words[i+1], "&") ||
                    sh_operator(words[i+1], ">") || sh_operator(words[i+1], ">>") || sh_operator(words[i+1], "<")) return -1;
                cur->out_path = words[++i];
                cur->append = app;
            }
        } else if (sh_operator(words[i], "&") || cur->argc >= MAX_ARGS) {
            return -1;
        } else {
            cur->argv[cur->argc++] = words[i];
            cur->argv[cur->argc] = 0;
        }
    }
    return st[ns - 1].argc ? ns : -1;
}

/*
 * ---- 背景工作 ----
 *
 * 命令最後加上 "&" 就在背景執行:shell 建立行程之後不等它,直接讀下一個命令。
 * 一條在背景執行的管線稱為一個「工作」(job),shell 記住它的每個行程,給它一個小編號。
 *
 * 和前景的命令相比,差別只有三點:
 *   1. 不設為主控台的前景群組,所以 Ctrl-C 不會終止它
 *   2. 標準輸入接到 /dev/null,不然它會和 shell 搶著讀鍵盤
 *   3. 它結束時沒有人正在等。shell 每次要讀下一個命令之前,用「不等待的 waitpid」
 *      看看有沒有背景的行程結束了,有就回收並印出通知
 * 輸出照常印到螢幕上(可能穿插在你正在打的字中間;不想看到就用 > 導到檔案)。
 *
 * 暫停:前景的命令被 Ctrl-Z 暫停時,kernel 讓等待中的 waitpid(WUNTRACED)返回,
 * shell 把它登記成一個「暫停中」的工作,回到提示符號。之後
 *   fg  送 SIGCONT 讓它繼續,並重新等它(回到前景)
 *   bg  送 SIGCONT 讓它繼續,但不等它(變成背景工作)
 * 背景工作如果去讀鍵盤,kernel 會把它暫停(SIGTTIN):鍵盤是留給前景用的。
 */
#define MAX_JOBS 8

struct job {
    int used;
    long pgid;                                   /* 行程群組:fg 時設為前景 */
    long pids[MAX_STAGES];                       /* 各個命令的行程;-1 = 已經結束 */
    int n, left;                                 /* 行程總數、還沒結束的個數 */
    int stopped;                                 /* 暫停中(值為造成暫停的訊號編號) */
    long status;                                 /* 最後一個命令的結束碼 */
    char cmd[64];                                /* 命令的文字,列表與通知時顯示 */
};

static struct job jobs[MAX_JOBS];

/* 把命令的文字組回來("ls /bin | wc"),太長就截斷 */
static void job_describe(struct job *j, struct stage *st, int ns)
{
    unsigned n = 0;

    for (int i = 0; i < ns; i++) {
        for (int k = 0; k < st[i].argc; k++) {
            const char *sep = k ? " " : i ? " | " : "";
            for (; *sep && n < sizeof(j->cmd) - 1; sep++)
                j->cmd[n++] = *sep;
            for (const char *c = st[i].argv[k]; *c && n < sizeof(j->cmd) - 1; c++)
                j->cmd[n++] = *c;
        }
    }
    j->cmd[n] = 0;
}

static void job_report(struct job *j, const char *what, long num)
{
    struct line l = { .len = 0 };

    line_str(&l, "[");
    line_num(&l, (long)(j - jobs) + 1);
    line_str(&l, "] ");
    line_str(&l, what);
    if (num >= 0)
        line_num(&l, num);
    line_str(&l, "  ");
    line_str(&l, j->cmd);
    line_str(&l, "\n");
    line_out(&l);
}

/* 記下「工作 j 的第 i 個行程結束了,結束碼 st」 */
static void job_mark_done(struct job *j, int i, int st)
{
    j->pids[i] = -1;
    j->left--;
    if (i == j->n - 1)
        j->status = st;                          /* 管線的結束碼 = 最後一個命令的 */
}

/* 找出行程 pid 屬於哪個工作,*idx 得到它是第幾個行程;不是任何工作的行程回傳 0 */
static struct job *job_of(long pid, int *idx)
{
    for (struct job *j = jobs; j < jobs + MAX_JOBS; j++) {
        for (int i = 0; j->used && i < j->n; i++) {
            if (j->pids[i] == pid) {
                *idx = i;
                return j;
            }
        }
    }
    return 0;
}

static void job_report_stopped(struct job *j)
{
    job_report(j, j->stopped == SIGTTIN ? "stopped (wants input; use fg)" : "stopped", -1);
}

/* 看看背景的行程有沒有結束或被暫停的(不等待);整個工作都結束的,印出通知並把位置空出來 */
static void jobs_reap(void)
{
    int st = 0, i = 0;
    long pid;

    while ((pid = sys_waitpid(-1, &st, WNOHANG | WUNTRACED)) > 0) {
        struct job *j = job_of(pid, &i);
        if (!j)
            continue;
        if (WIFSTOPPED(st)) {
            if (!j->stopped) {                   /* 同一個工作的其他行程也會各回報一次 */
                j->stopped = WSTOPSIG(st);
                job_report_stopped(j);
            }
            continue;
        }
        job_mark_done(j, i, st);
        if (j->left == 0) {
            if (j->status == 0)
                job_report(j, "done", -1);
            else
                job_report(j, "exit ", j->status);
            j->used = 0;
        }
    }
}

/*
 * 把工作放在前景,等到它「結束」或「被暫停」(此時 Ctrl-C 會終止它、Ctrl-Z 會暫停它)。
 * 暫停中的工作會先讓它繼續。回傳 0 = 結束了(結束碼在 j->status);1 = 被暫停了(j->stopped 已設定);
 * 2 = 等待失敗(一直被打斷,或 kernel 回報錯誤):工作與它的行程都原樣保留,沒有被標成結束或暫停 —
 * 它可能還在執行,呼叫者不可以把它從表格拿掉。
 * j 可以是表格裡的工作,也可以是呼叫者手上暫時的一個(前景命令一開始還不算工作)。
 */
/* 把標準輸入 / 輸出換回原本的。換不回來的話 shell 已經沒有可以用的輸入輸出了:
   繼續執行只會把之後的命令接到錯的地方,所以直接結束 */
static void restore_fd(int saved, int fd)
{
    if (sys_dup2(saved, fd) != fd)
        sys_exit(1);
}

static int job_wait(struct job *j)
{
    sys_tcsetfg(j->pgid);                        /* 先設為前景再讓它繼續:它也許正等著讀鍵盤 */
    if (j->stopped) {
        long rc = sys_kill(-j->pgid, SIGCONT);
        if (rc != 0 && rc != -E_SRCH) {
            /* 叫不動它:它還停著,這時候去等只會把 shell 自己卡住。維持「暫停」的狀態回去 */
            puts_("sh: cannot continue the job\n");
            sys_tcsetfg(0);                      /* 把前景還給 shell 自己:不然接下來讀鍵盤的 shell 反而成了「背景」 */
            return 1;
        }
        j->stopped = 0;                          /* -E_SRCH = 整組都不在了:下面的等待會立刻收到結果 */
    }
    for (int i = 0; i < j->n && !j->stopped; i++) {
        int st = 0;

        if (j->pids[i] < 0)
            continue;
        /* 等待被訊號打斷(-E_INTR)不是「它結束了」:再等。次數有上限,免得出了別的問題時永遠繞不出去 */
        long w = -E_INTR;
        for (int tries = 0; tries < 1000 && w == -E_INTR; tries++)
            w = sys_waitpid(j->pids[i], &st, WUNTRACED);
        if (w != j->pids[i]) {
            /* 等不到。不可以當成「它結束了」:它可能還活著,把它從紀錄裡拿掉就再也沒有人管得到它。
               保留這個行程與整個工作,讓呼叫者知道這次沒有等成功(回傳 2) */
            sys_tcsetfg(0);
            return 2;
        }
        if (WIFSTOPPED(st))
            j->stopped = WSTOPSIG(st);           /* 整個群組一起被暫停,其餘的不必再等 */
        else
            job_mark_done(j, i, st);
    }
    sys_tcsetfg(0);
    return j->stopped != 0;
}

static struct job *job_free_slot(void)
{
    for (struct job *j = jobs; j < jobs + MAX_JOBS; j++)
        if (!j->used)
            return j;
    return 0;
}

/* 內建命令 fg / bg 的參數:指定編號,或(沒有給)編號最大的那個;
   only_stopped 時只考慮暫停中的。找不到回傳 0 */
static struct job *job_pick(const char *arg, int only_stopped)
{
    struct job *j = 0;

    for (struct job *k = jobs; k < jobs + MAX_JOBS; k++) {
        if (!k->used || (only_stopped && !k->stopped))
            continue;
        if (!arg || uatoi(arg) == (k - jobs) + 1)
            j = k;
    }
    return j;
}

static void complain(const char *what, const char *name)
{
    puts_("sh: ");
    puts_(what);
    puts_(name);
    puts_("\n");
}

/*
 * 執行一條管線(只有一個命令時就是一般的執行),回傳最後一個命令的結束碼。
 * 找不到程式時是 127,和 Unix 的慣例相同。
 *
 * 輸入輸出的安排靠一個性質:新行程會繼承我們的 0、1、2 號描述符。
 * 所以在建立每個命令之前,把「我們自己的」標準輸入 / 輸出暫時換成它該用的東西
 * (檔案,或管線的一端),建立完再換回來。被執行的程式完全不知情,
 * 照常讀 0 號、寫 1 號。
 *
 * 管線的每一段:
 *     命令 i 的輸出 ──▶ [管線 i] ──▶ 命令 i+1 的輸入
 * 建立命令 i 之後,我們自己要立刻關掉管線 i 的寫端:只要還有人開著寫端,
 * 讀的那一方就等不到「結束」。讀端則留到建立命令 i+1 時交給它,之後也關掉。
 */
static long run(struct stage *st, int ns, int background)
{
    long pids[MAX_STAGES];
    long prev_rd = -1;
    long keep = -1;                              /* SAVE_SYNC:輸出檔的另一個參考,見下面「確認寫入」 */
    const char *keep_path = 0;
    struct job *job = 0;

    /* 先全部設成「沒有」:中途失敗提早離開迴圈時,後面還沒輪到的那幾格才不會是未初始化的值 */
    for (int i = 0; i < MAX_STAGES; i++)
        pids[i] = -1;

    if (background) {
        job = job_free_slot();
        if (!job) {
            puts_("sh: too many background jobs\n");
            return 1;
        }
        if (!st[0].in_path)
            st[0].in_path = "/dev/null";         /* 背景的命令不讀鍵盤 */
    }

    for (int i = 0; i < ns; i++) {
        char path[128] = "/bin/";
        const char *prog = st[i].argv[0];
        int has_slash = 0, fds[2] = { -1, -1 };
        long fd;

        pids[i] = -1;
        for (const char *c = prog; *c; c++)
            has_slash |= *c == '/';
        if (!has_slash) {
            unsigned n = 5;
            for (const char *c = prog; *c && n < sizeof(path) - 1; c++)
                path[n++] = *c;
            path[n] = 0;
            prog = path;
        }
        if (i + 1 < ns && sys_pipe(fds) != 0) {
            puts_("sh: cannot create a pipe\n");
            break;
        }

        /* 記住原本的標準輸入 / 輸出。記不住就不可以去動 0 和 1:之後換不回來,shell 自己就壞了 */
        int ok = 1;
        int saved_in = sys_dup2(0, SAVE_IN) == SAVE_IN;
        if (!saved_in || sys_dup2(1, SAVE_OUT) != SAVE_OUT) {
            puts_("sh: cannot save the standard input/output\n");
            if (saved_in)
                close_quiet(SAVE_IN);              /* 第一個存成功、第二個失敗:自己開的那一個要關掉 */
            if (prev_rd >= 0)
                close_quiet(prev_rd);
            if (fds[0] >= 0)
                close_quiet(fds[0]);
            if (fds[1] >= 0)
                close_quiet(fds[1]);
            prev_rd = -1;
            break;
        }

        if (prev_rd >= 0) {                      /* 輸入:上一個命令的管線 */
            if (sys_dup2(prev_rd, 0) != 0)
                ok = 0;                          /* 接不上:不可以讓命令去讀原本的輸入 */
            close_quiet(prev_rd);
            prev_rd = -1;
        } else if (st[i].in_path) {              /* 或檔案 */
            fd = sys_open(st[i].in_path, O_RDONLY);
            if (fd < 0) {
                complain("cannot open ", st[i].in_path);
                ok = 0;
            } else {
                if (sys_dup2(fd, 0) != 0)
                    ok = 0;
                close_quiet(fd);
            }
        }
        if (fds[1] >= 0) {                       /* 輸出:接到下一個命令的管線 */
            if (sys_dup2(fds[1], 1) != 1)
                ok = 0;                          /* 接不上:不可以讓命令把輸出印到畫面上 */
            close_quiet(fds[1]);                   /* 我們自己不留寫端 */
        } else if (st[i].out_path) {             /* 或檔案 */
            fd = sys_open(st[i].out_path,
                          O_CREAT | O_WRONLY | (st[i].append ? O_APPEND : O_TRUNC));
            if (fd < 0) {
                restore_fd(SAVE_OUT, 1);         /* 先換回來,錯誤訊息才印得出來 */
                complain("cannot open ", st[i].out_path);
                ok = 0;
            } else {
                if (sys_dup2(fd, 1) != 1)
                    ok = 0;
                /* 自己另外留一個指向同一個檔案的描述符:命令寫完之後才能替它確認資料寫成功了。
                   (子行程只繼承 0、1、2 號,不會拿到這一個) */
                close_quiet(keep);
                keep = sys_dup2(fd, SAVE_SYNC) == SAVE_SYNC ? SAVE_SYNC : -1;
                if (keep < 0)
                    ok = 0;
                keep_path = st[i].out_path;
                close_quiet(fd);
            }
        }

        if (ok)
            pids[i] = sys_spawn(prog, st[i].argv);

        restore_fd(SAVE_IN, 0);                  /* 換回來 */
        restore_fd(SAVE_OUT, 1);
        if (!ok && pids[i] < 0 && i + 1 < ns)
            complain("cannot redirect for ", st[i].argv[0]);
        close_quiet(SAVE_IN);
        close_quiet(SAVE_OUT);
        prev_rd = fds[0];                        /* 留給下一個命令當輸入 */

        if (ok && pids[i] < 0)
            complain(st[i].argv[0],
                     pids[i] == -E_NOENT ? ": command not found" :
                     pids[i] == -E_ACCES ? ": permission denied" : ": cannot execute");
    }
    if (prev_rd >= 0)
        close_quiet(prev_rd);

    /*
     * 把這條管線的所有行程放進同一個行程群組,並設為主控台的「前景」:
     * 使用者按 Ctrl-C 時,kernel 會終止前景群組的所有行程 — 也就是整條管線,
     * 而不是 shell 自己。等它們都結束後設回「沒有前景」,
     * 所以在提示符號下按 Ctrl-C 只會放棄正在打的那一行。
     */
    long pgid = 0;
    for (int i = 0; i < ns; i++) {
        if (pids[i] < 0)
            continue;
        if (!pgid)
            pgid = pids[i];                      /* 以第一個行程的編號當群組編號 */
        sys_setpgid(pids[i], pgid);
    }
    /* 不論前景或背景,先整理成一個工作:記下群組與每個成功啟動的行程 */
    struct job cur = { .used = 1, .pgid = pgid };
    for (int i = 0; i < ns; i++) {
        if (pids[i] < 0)
            continue;
        cur.pids[cur.n++] = pids[i];
        cur.left++;
    }
    job_describe(&cur, st, ns);

    if (background) {
        /* 背景:記下這個工作就回去讀下一個命令。一個行程都沒啟動成功的話不算工作 */
        close_quiet(keep);                      /* 背景工作:不等它,也就無從替它確認 */
        if (!pgid)
            return 127;
        *job = cur;
        job_report(job, "started, pid ", job->pids[job->n - 1]);
        return 0;
    }

    /* 前景:全部建立完才開始等(它們是同時在執行的) */
    int w;
    while (pgid && (w = job_wait(&cur)) != 0) {
        /* 1 = 被 Ctrl-Z 暫停了;2 = 等待失敗(它可能還在執行)。兩種都登記成工作、回到提示符號:
           之後可以用 jobs / fg / wait 再處理。等待失敗的不標成「暫停」— 那不是事實 */
        job = job_free_slot();
        if (job) {
            *job = cur;
            if (w == 2)
                job_report(job, "could not wait for it; still listed as a job, pid ", job->pids[job->n - 1]);
            else
                job_report_stopped(job);
            close_quiet(keep);                  /* 它還沒寫完 */
            return w == 2 ? 1 : 128 + cur.stopped;
        }
        if (w == 2) {
            puts_("sh: could not wait for the command, and the job table is full\n");
            close_quiet(keep);
            return 1;
        }
        /* 表格滿了就沒辦法記住被暫停的工作,只好讓它繼續、接著等
           (否則它會停在那裡,再也沒有人能讓它繼續) */
        puts_("sh: too many jobs; cannot stop this one\n");
    }
    /*
     * 確認寫入:命令都結束了,這時候才把「> 檔案」的那個檔案同步到磁碟,並檢查結果。
     * 要在命令結束「之後」做(之前檔案還是空的),而且同步的必須是命令實際寫入的那個檔案
     * (不是已經換回來的標準輸出 — 那是主控台)。
     * 寫不成功的話,命令本身即使回報成功,整行也算失敗:使用者以為存好的東西其實沒有存好。
     */
    int save_failed = 0;
    if (keep >= 0) {
        long synced = sys_fsync(keep);
        long closed = sys_close(keep);           /* 不論成不成功,描述符都已經關閉 */
        if (synced == -E_INVAL)
            synced = 0;                          /* 「> /dev/null」這類:對象沒有寫回這件事,不是錯誤 */
        if (synced != 0 || closed != 0) {
            complain("write error: could not save ", keep_path);
            save_failed = 1;
        }
    }
    /* 最後一個命令沒有啟動:重新導向失敗 1、找不到程式 127;否則是它的結束碼 */
    if (pids[ns - 1] < 0)
        return pids[ns - 1] == -1 ? 1 : 127;
    return save_failed && cur.status == 0 ? 1 : cur.status;
}

int main(int argc, char **argv)
{
    struct reader in = { .fd = 0 };
    int interactive = argc < 2;
    char linebuf[256];
    char *words[MAX_ARGS * MAX_STAGES + 1];
    static struct stage stages[MAX_STAGES];
    long status = 0;

    if (!interactive) {
        in.fd = sys_open(argv[1], O_RDONLY);
        if (in.fd < 0) {
            complain("cannot open ", argv[1]);
            return 127;
        }
    }

    for (;;) {
        jobs_reap();                             /* 有背景工作結束了的話,在提示符號之前通知 */
        if (interactive) {
            /* 提示符號:在根目錄時是 "$ ",其他地方前面加上目前的目錄("/disk$ ") */
            char cwd[200];
            long uid = sys_getuid();
            if (uid != 0)                        /* 不是 root:顯示目前是誰(su 之後看得出來) */
                print_num("[uid ", uid, "] ");
            if (sys_getcwd(cwd, sizeof(cwd)) > 1)
                puts_(cwd);
            puts_("$ ");
        }
        int got = interactive ? sh_read_edit(&in, linebuf, sizeof(linebuf)) :
                                read_line(&in, linebuf, sizeof(linebuf));
        if (got < 0) { puts_("sh: line too long\n"); status = 2; continue; }
        if (!got)
            break;                               /* 輸入結束 */
        int n = sh_lex(linebuf, words, status);
        if (n < 0) { puts_("sh: invalid or oversized command\n"); status = 2; continue; }
        if (n == 0)
            continue;                            /* 空行或只有註解 */

        int background = 0;
        if (sh_operator(words[n - 1], "&")) {
            background = 1;
            words[--n] = 0;
            if (n == 0) { puts_("sh: syntax error\n"); status = 2; continue; }
        }
        if (n == 1 && !background && (sh_kind[0] & SH_ASSIGN)) {
            status = sh_assign(words[0]) == 1 ? 0 : 2;
            if (status) puts_("sh: variable table or value too large\n");
            continue;
        }
        n = sh_expand(words, n);
        if (n < 0) { puts_("sh: pathname expansion failed or ambiguous redirect\n"); status = 2; continue; }

        if (streq(words[0], "exit")) {
            return n > 1 ? (int)uatoi(words[1]) : (int)status;
        } else if (streq(words[0], "cd")) {
            long rc = sys_chdir(n > 1 ? words[1] : "/");
            if (rc != 0)
                complain(rc == -E_NOTDIR ? "cd: not a directory: " : "cd: no such directory: ",
                         n > 1 ? words[1] : "/");
            status = rc != 0;
        } else if (streq(words[0], "pwd")) {
            char cwd[256];
            if (sys_getcwd(cwd, sizeof(cwd)) > 0) {
                puts_(cwd);
                puts_("\n");
            }
            status = 0;
        } else if (streq(words[0], "limit")) {
            long kb = sys_memlimit(n > 1 ? uatoi(words[1]) : 0);
            if (kb < 0)
                puts_("sh: limit: cannot raise the limit (lowering it is always allowed)\n");
            else
                print_num("memory limit: ", kb, " KB per process\n");
            status = kb < 0;
        } else if (streq(words[0], "status")) {
            print_num("", status, "\n");
            status = 0;
        } else if (streq(words[0], "help")) {
            puts_("built-in: cd [dir], pwd, limit [KB], exit [code], status, help, jobs, fg [n], bg [n], wait\n"
                  "programs: anything in /bin (try: ls /bin), e.g. echo, cat, ls, wc, sort, date, mkdir, rm, cp, mv, sleep, kill, ps, top, df, sync\n"
                  "network: net, ping 10.0.2.2, host NAME, tftp REMOTE-FILE [LOCAL-FILE] (files come from the host)\n"
                  "reliability: faultlog (events kept across reboots), supervise COMMAND (restart on failure), scrub (verify the disk),\n"
                  "             periodic [PERIOD_MS BUDGET_MS COUNT WORK_MS] (run as a real-time periodic task),\n"
                  "             vote COMMAND (run three copies and take the majority result)\n"
                  "users: id, su USER (exit returns), chmod MODE FILE, chown USER FILE; ls -l shows owners\n"
                  "variables: NAME=value, $NAME, ${NAME}, $? (shell-local; no export)\n"
                  "quotes: single/double quotes and backslash; glob: * and ? in paths\n"
                  "redirection: command > file, command >> file, command < file\n"
                  "pipes: command | command | ...\n"
                  "background: command &   then: jobs, fg [n], wait; ps lists all processes\n"
                  "Ctrl-C ends the running command; Ctrl-Z stops it (then: fg to resume, bg to resume in background)\n"
                  "editing: Left/Right, Home/End, Delete, Up/Down history, Tab filename completion\n"
                  "Ctrl-D at an empty prompt ends the shell\n"
                  "disk: files under /disk are kept across reboots (try: df, cp /bin/echo /disk/echo)\n");
            status = 0;
        } else if (streq(words[0], "jobs")) {
            for (struct job *j = jobs; j < jobs + MAX_JOBS; j++) {
                if (j->used && j->stopped)
                    job_report_stopped(j);
                else if (j->used)
                    job_report(j, "running", -1);
            }
            status = 0;
        } else if (streq(words[0], "fg")) {
            struct job *j = job_pick(n > 1 ? words[1] : 0, 0);
            if (!j) {
                puts_("sh: fg: no such job\n");
                status = 1;
            } else {
                job_report(j, "foreground", -1);
                int w = job_wait(j);
                if (w == 2) {                    /* 等待失敗:留在表格裡,不假裝它結束或暫停了 */
                    puts_("sh: fg: could not wait for the job; it is still listed\n");
                    status = 1;
                } else if (w) {                  /* 又被暫停了:留在表格裡 */
                    job_report_stopped(j);
                    status = 128 + j->stopped;
                } else {
                    status = j->status;
                    j->used = 0;
                }
            }
        } else if (streq(words[0], "bg")) {
            struct job *j = job_pick(n > 1 ? words[1] : 0, 1);
            if (!j) {
                puts_("sh: bg: no stopped job\n");
                status = 1;
            } else {
                j->stopped = 0;
                if (sys_kill(-j->pgid, SIGCONT) != 0) {
                    puts_("sh: bg: the job is gone\n");     /* 剛好已經結束了:下一輪會回報 */
                    status = 1;
                } else {
                    job_report(j, "continued", -1);
                    status = 0;
                }
            }
        } else if (streq(words[0], "wait")) {
            /* 暫停中的工作不會自己結束,不等它們 */
            for (struct job *j = jobs; j < jobs + MAX_JOBS; j++) {
                if (!j->used || j->stopped)
                    continue;
                int w = job_wait(j);
                if (w == 2) {
                    puts_("sh: wait: could not wait for a job; it is still listed\n");
                    status = 1;
                } else if (w) {
                    job_report_stopped(j);
                } else {
                    status = j->status;
                    j->used = 0;
                }
            }
        } else {
            int ns = parse(words, n, stages);
            if (ns < 0) {
                puts_("sh: syntax error\n");
                status = 2;
            } else {
                status = run(stages, ns, background);
            }
        }
    }
    return (int)status;
}
