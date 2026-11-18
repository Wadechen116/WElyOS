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
 * shell 的結束碼:exit 指定的值,或(輸入結束時)最後一個命令的結束碼。
 */
#include "ulib.h"

#define MAX_ARGS   15
#define MAX_STAGES 6                             /* 一條管線最多由幾個命令組成 */
#define SAVE_IN    14                            /* 重新導向時暫存原本標準輸入 / 輸出的描述符編號 */
#define SAVE_OUT   15

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

    for (;;) {
        if (r->pos >= r->len) {
            r->len = sys_read(r->fd, r->buf, sizeof(r->buf));
            r->pos = 0;
            if (r->len <= 0) {                   /* 輸入結束(或出錯) */
                out[n] = 0;
                return n > 0;                    /* 最後一行沒有換行結尾時仍然算一行 */
            }
        }
        char c = r->buf[r->pos++];
        if (c == '\n') {
            out[n] = 0;
            return 1;
        }
        if (n < cap - 1)
            out[n++] = c;
    }
}

/* 把一行就地切成參數:在每個字的結尾寫入 0,argv 指向各個字的開頭。回傳參數個數 */
static int split(char *s, char **argv, int max)
{
    int argc = 0;

    while (*s && argc < max) {
        while (*s == ' ' || *s == '\t')
            s++;
        if (!*s || *s == '#')                    /* 行尾,或註解的開頭 */
            break;
        if (*s == '"') {                         /* 引號:到下一個引號為止算同一個字 */
            argv[argc++] = ++s;
            while (*s && *s != '"')
                s++;
        } else {
            argv[argc++] = s;
            while (*s && *s != ' ' && *s != '\t')
                s++;
        }
        if (*s)
            *s++ = 0;
    }
    argv[argc] = 0;
    return argc;
}

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
        int out = streq(words[i], ">"), app = streq(words[i], ">>"), in = streq(words[i], "<");

        if (streq(words[i], "|")) {
            if (cur->argc == 0 || ns >= MAX_STAGES || i + 1 >= n)
                return -1;                       /* "|" 的前後都要有命令 */
            st[ns++] = (struct stage){ .argc = 0 };
        } else if (out || app || in) {
            if (i + 1 >= n)
                return -1;                       /* 後面要接檔名 */
            if (in) {
                cur->in_path = words[++i];
            } else {
                cur->out_path = words[++i];
                cur->append = app;
            }
        } else if (cur->argc < MAX_ARGS) {
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
 * 暫停中的工作會先讓它繼續。回傳 1 = 被暫停了(j->stopped 已設定),0 = 結束了(結束碼在 j->status)。
 * j 可以是表格裡的工作,也可以是呼叫者手上暫時的一個(前景命令一開始還不算工作)。
 */
static int job_wait(struct job *j)
{
    sys_tcsetfg(j->pgid);                        /* 先設為前景再讓它繼續:它也許正等著讀鍵盤 */
    if (j->stopped) {
        j->stopped = 0;
        sys_kill(-j->pgid, SIGCONT);
    }
    for (int i = 0; i < j->n && !j->stopped; i++) {
        int st = 0;

        if (j->pids[i] < 0)
            continue;
        if (sys_waitpid(j->pids[i], &st, WUNTRACED) != j->pids[i])
            st = 1;                              /* 等不到(不該發生):當作它已經結束 */
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
    struct job *job = 0;

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

        sys_dup2(0, SAVE_IN);                    /* 記住原本的標準輸入 / 輸出 */
        sys_dup2(1, SAVE_OUT);
        int ok = 1;

        if (prev_rd >= 0) {                      /* 輸入:上一個命令的管線 */
            sys_dup2(prev_rd, 0);
            sys_close(prev_rd);
            prev_rd = -1;
        } else if (st[i].in_path) {              /* 或檔案 */
            fd = sys_open(st[i].in_path, O_RDONLY);
            if (fd < 0) {
                complain("cannot open ", st[i].in_path);
                ok = 0;
            } else {
                sys_dup2(fd, 0);
                sys_close(fd);
            }
        }
        if (fds[1] >= 0) {                       /* 輸出:接到下一個命令的管線 */
            sys_dup2(fds[1], 1);
            sys_close(fds[1]);                   /* 我們自己不留寫端 */
        } else if (st[i].out_path) {             /* 或檔案 */
            fd = sys_open(st[i].out_path,
                          O_CREAT | O_WRONLY | (st[i].append ? O_APPEND : O_TRUNC));
            if (fd < 0) {
                sys_dup2(SAVE_OUT, 1);           /* 先換回來,錯誤訊息才印得出來 */
                complain("cannot open ", st[i].out_path);
                ok = 0;
            } else {
                sys_dup2(fd, 1);
                sys_close(fd);
            }
        }

        if (ok)
            pids[i] = sys_spawn(prog, st[i].argv);

        sys_dup2(SAVE_IN, 0);                    /* 換回來 */
        sys_dup2(SAVE_OUT, 1);
        sys_close(SAVE_IN);
        sys_close(SAVE_OUT);
        prev_rd = fds[0];                        /* 留給下一個命令當輸入 */

        if (ok && pids[i] < 0)
            complain(st[i].argv[0],
                     pids[i] == -E_NOENT ? ": command not found" :
                     pids[i] == -E_ACCES ? ": permission denied" : ": cannot execute");
    }
    if (prev_rd >= 0)
        sys_close(prev_rd);

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
        if (!pgid)
            return 127;
        *job = cur;
        job_report(job, "started, pid ", job->pids[job->n - 1]);
        return 0;
    }

    /* 前景:全部建立完才開始等(它們是同時在執行的) */
    while (pgid && job_wait(&cur)) {
        /* 被 Ctrl-Z 暫停了:登記成工作,回到提示符號。表格滿了就沒辦法記住它,
           只好讓它繼續、接著等(否則它會停在那裡,再也沒有人能讓它繼續) */
        job = job_free_slot();
        if (job) {
            *job = cur;
            job_report_stopped(job);
            return 128 + cur.stopped;
        }
        puts_("sh: too many jobs; cannot stop this one\n");
    }
    /* 最後一個命令沒有啟動:重新導向失敗 1、找不到程式 127;否則是它的結束碼 */
    if (pids[ns - 1] < 0)
        return pids[ns - 1] == -1 ? 1 : 127;
    return cur.status;
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
        if (!read_line(&in, linebuf, sizeof(linebuf)))
            break;                               /* 輸入結束 */
        int n = split(linebuf, words, MAX_ARGS * MAX_STAGES);
        if (n == 0)
            continue;                            /* 空行或只有註解 */

        /* 最後是 "&"(單獨一個字,或黏在最後一個字後面):背景執行 */
        int background = 0;
        char *last = words[n - 1];
        unsigned long ll = ustrlen(last);
        if (last[ll - 1] == '&') {
            background = 1;
            last[ll - 1] = 0;
            if (ll == 1)
                words[--n] = 0;
            if (n == 0) {
                puts_("sh: syntax error\n");
                status = 2;
                continue;
            }
        }

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
                  "network: net, ping 10.0.2.2, tftp REMOTE-FILE [LOCAL-FILE] (files come from the host)\n"
                  "users: id, su USER (exit returns), chmod MODE FILE, chown USER FILE; ls -l shows owners\n"
                  "redirection: command > file, command >> file, command < file\n"
                  "pipes: command | command | ...\n"
                  "background: command &   then: jobs, fg [n], wait; ps lists all processes\n"
                  "Ctrl-C ends the running command; Ctrl-Z stops it (then: fg to resume, bg to resume in background)\n"
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
                if (job_wait(j)) {               /* 又被暫停了:留在表格裡 */
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
                sys_kill(-j->pgid, SIGCONT);
                job_report(j, "continued", -1);
                status = 0;
            }
        } else if (streq(words[0], "wait")) {
            /* 暫停中的工作不會自己結束,不等它們 */
            for (struct job *j = jobs; j < jobs + MAX_JOBS; j++) {
                if (!j->used || j->stopped)
                    continue;
                if (job_wait(j)) {
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
