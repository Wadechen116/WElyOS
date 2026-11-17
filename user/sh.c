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
 *   "含 空白 的 參數"       雙引號內的空白不拆開
 *   # 註解                  這一行從 # 起到結尾都忽略
 * 內建命令(由 shell 自己處理,不建立行程):
 *   exit [數字]             結束 shell;沒有給數字時以上一個命令的結束碼結束
 *   status                  印出上一個命令的結束碼
 *   help                    列出可用的命令
 *
 * shell 的結束碼:exit 指定的值,或(輸入結束時)最後一個命令的結束碼。
 */
#include "ulib.h"

#define MAX_ARGS 15
#define SAVED_FD 15                              /* 重新導向時暫存原本標準輸出的描述符編號 */

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
static int split(char *s, char **argv)
{
    int argc = 0;

    while (*s && argc < MAX_ARGS) {
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

/* 執行一個外部命令,回傳它的結束碼(找不到程式回傳 127,和 Unix 的慣例相同) */
static long run(char **argv, const char *out_path, int append)
{
    char path[128] = "/bin/";
    const char *prog = argv[0];
    int has_slash = 0;

    for (const char *c = prog; *c; c++)
        has_slash |= *c == '/';
    if (!has_slash) {
        unsigned n = 5;
        for (const char *c = prog; *c && n < sizeof(path) - 1; c++)
            path[n++] = *c;
        path[n] = 0;
        prog = path;
    }

    /*
     * 重新導向:新行程會繼承我們開啟的檔案,所以只要在建立它之前,
     * 把「我們自己的」標準輸出暫時換成那個檔案,建立完再換回來。
     * 被執行的程式完全不知道這件事,它照常寫入 1 號描述符。
     */
    if (out_path) {
        long fd = sys_open(out_path, O_CREAT | O_WRONLY | (append ? O_APPEND : O_TRUNC));
        if (fd < 0) {
            puts_("sh: cannot open ");
            puts_(out_path);
            puts_("\n");
            return 1;
        }
        sys_dup2(1, SAVED_FD);                   /* 記住原本的標準輸出 */
        sys_dup2(fd, 1);
        sys_close(fd);
    }

    long pid = sys_spawn(prog, argv);

    if (out_path) {
        sys_dup2(SAVED_FD, 1);                   /* 換回來 */
        sys_close(SAVED_FD);
    }
    if (pid < 0) {
        puts_("sh: ");
        puts_(argv[0]);
        puts_(pid == -E_NOENT ? ": command not found\n" : ": cannot execute\n");
        return 127;
    }
    return sys_wait(pid);
}

int main(int argc, char **argv)
{
    struct reader in = { .fd = 0 };
    int interactive = argc < 2;
    char linebuf[256];
    char *args[MAX_ARGS + 1];
    long status = 0;

    if (!interactive) {
        in.fd = sys_open(argv[1], O_RDONLY);
        if (in.fd < 0) {
            puts_("sh: cannot open ");
            puts_(argv[1]);
            puts_("\n");
            return 127;
        }
    }

    for (;;) {
        if (interactive)
            puts_("$ ");
        if (!read_line(&in, linebuf, sizeof(linebuf)))
            break;                               /* 輸入結束 */
        int n = split(linebuf, args);
        if (n == 0)
            continue;                            /* 空行或只有註解 */

        /* 找出重新導向:"> 檔案" 或 ">> 檔案",並把它們從參數中拿掉 */
        const char *out_path = 0;
        int append = 0, bad = 0;
        for (int i = 0; i < n; i++) {
            int app = streq(args[i], ">>");
            if (!app && !streq(args[i], ">"))
                continue;
            if (i + 1 >= n) {
                puts_("sh: missing file name after redirection\n");
                bad = 1;
                break;
            }
            out_path = args[i + 1];
            append = app;
            for (int k = i; k + 2 <= n; k++)     /* 後面的往前移兩格(含結尾的 NULL) */
                args[k] = args[k + 2];
            n -= 2;
            i--;
        }
        if (bad || n == 0) {
            status = 2;
            continue;
        }

        if (streq(args[0], "exit")) {
            return n > 1 ? (int)uatoi(args[1]) : (int)status;
        } else if (streq(args[0], "status")) {
            print_num("", status, "\n");
            status = 0;
        } else if (streq(args[0], "help")) {
            puts_("built-in: exit [code], status, help\n"
                  "programs: anything in /bin (try: ls /bin), e.g. echo, cat, ls, mkdir, rm\n"
                  "redirection: command > file, command >> file\n");
            status = 0;
        } else {
            status = run(args, out_path, append);
        }
    }
    return (int)status;
}
