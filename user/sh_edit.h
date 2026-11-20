/*
 * sh_edit.h - shell 的行編輯(Codex 第 51 步交付):以位元組為單位編輯(不是 Unicode 字元),
 * 記憶體中保留 16 筆歷史。編輯時主控台用 raw 模式,執行任何命令之前一定還原成一般(canonical)模式。
 */
#ifndef WELYOS_SH_EDIT_H
#define WELYOS_SH_EDIT_H
#define SH_HISTORY 16
static char sh_history[SH_HISTORY][SH_TEXT];
static unsigned sh_history_count;
static void sh_copy(char *dst, const char *src)
{
    unsigned i = 0;
    do { dst[i] = src[i]; } while (src[i++]);
}
static void sh_history_add(const char *line)
{
    if (!*line || (sh_history_count && streq(line, sh_history[sh_history_count - 1]))) return;
    if (sh_history_count == SH_HISTORY) {
        for (unsigned i = 1; i < SH_HISTORY; i++) sh_copy(sh_history[i-1], sh_history[i]);
        sh_history_count--;
    }
    sh_copy(sh_history[sh_history_count++], line);
}
static void sh_redraw(const char *s, unsigned oldpos, unsigned oldlen, unsigned pos)
{
    for (unsigned i = 0; i < oldpos; i++) puts_("\b");
    puts_(s);
    unsigned len = (unsigned)ustrlen(s), end = len > oldlen ? len : oldlen;
    for (unsigned i = len; i < oldlen; i++) puts_(" ");
    for (unsigned i = pos; i < end; i++) puts_("\b");
}
/* Tab 補全:只補「沒有引號、沒有特殊符號」的字,遇到引號或語法符號就不補(不去改寫它的意思)。
   游標必須在行尾;只有一個符合的目錄時在後面補上 /。 */
static int sh_complete(char *line, unsigned *cursor, unsigned cap)
{
    unsigned start = *cursor;
    while (start && line[start-1] != ' ' && line[start-1] != '\t' && line[start-1] != '|') start--;
    if (*cursor != ustrlen(line)) return 0;
    unsigned len = *cursor - start;
    if (!len) return 0;
    char dir[SH_TEXT], prefix[SH_TEXT], common[SH_TEXT];
    unsigned slash = 0;
    for (unsigned i = start; i < *cursor; i++) {
        char c = line[i];
        if (c == '"' || c == '\'' || c == '\\' || c == '$' || c == '*' || c == '?' || c == '<' || c == '>' || c == '&') return 0;
        if (c == '/') slash = i - start + 1;
    }
    unsigned pn = len - slash;
    for (unsigned i = 0; i < pn; i++) prefix[i] = line[start + slash + i];
    prefix[pn] = 0;
    int command = 1;
    for (unsigned i = 0; i < start; i++)
        if (line[i] != ' ' && line[i] != '\t') command = 0;
    /* 管線符號之後的第一個字也是命令 */
    unsigned prev = start;
    while (prev && (line[prev-1] == ' ' || line[prev-1] == '\t')) prev--;
    if (prev && line[prev-1] == '|') command = 1;
    if (slash) {
        for (unsigned i = 0; i < slash; i++) dir[i] = line[start+i];
        dir[slash] = 0;
    } else sh_copy(dir, command ? "/bin" : ".");
    long fd = sys_open(dir, O_RDONLY);
    if (fd < 0) return -1;
    struct dirent e; unsigned matches = 0; int isdir = 0, err = 0;
    for (;;) {
        long rc = sys_readdir(fd, &e);
        if (rc == 1) break;
        if (rc != 0) { err = -1; break; }
        if (e.name[0] == '.' && prefix[0] != '.') continue;
        unsigned j = 0;
        while (j < pn && e.name[j] == prefix[j]) j++;
        if (j != pn) continue;
        if (!matches) { sh_copy(common, e.name); isdir = e.type == I_DIR; }
        else {
            j = 0;
            while (common[j] && common[j] == e.name[j]) j++;
            common[j] = 0;
        }
        matches++;
    }
    if (sys_close(fd) != 0) err = -1;
    if (err || !matches) return err;
    char addition[SH_TEXT]; unsigned an = 0;
    for (unsigned j = pn; common[j]; j++) {
        char c = common[j];
        if (c == ' ' || c == '\t' || c == '\\' || c == '\'' || c == '"' || c == '$' || c == '*' || c == '?' || c == '#' || c == '|' || c == '<' || c == '>' || c == '&') {
            if (an + 1 >= sizeof(addition)) return -1;
            addition[an++] = '\\';
        }
        if (an + 1 >= sizeof(addition)) return -1;
        addition[an++] = c;
    }
    if (matches == 1) {
        if (an + 1 >= sizeof(addition)) return -1;
        addition[an++] = isdir ? '/' : ' ';
    }
    if (*cursor + an >= cap) return -1;
    for (unsigned j = 0; j < an; j++) line[(*cursor)++] = addition[j];
    line[*cursor] = 0;
    return 0;
}
static int sh_read_edit(struct reader *r, char *out, unsigned cap)
{
    if (sys_tcsetmode(CON_RAW | CON_NOECHO) != 0) return read_line(r, out, cap);
    unsigned n = 0, pos = 0, history = sh_history_count;
    char draft[SH_TEXT]; draft[0] = 0; out[0] = 0;
    int escape = 0, result = 1;
    for (;;) {
        char c;
        long got = sys_read(r->fd, &c, 1);
        if (got == -E_INTR) continue;
        if (got <= 0) { result = n != 0; break; }
        unsigned oldpos = pos, oldlen = n;
        if (escape) {
            if (escape == 1) { escape = c == '[' || c == 'O' ? 2 : 0; continue; }
            if (escape == 3) {
                escape = 0;
                if (c != '~') continue;
                if (pos < n) { for (unsigned i = pos; i < n; i++) out[i] = out[i+1]; n--; }
            } else {
                escape = 0;
                if (c == 'D' && pos) pos--;
                else if (c == 'C' && pos < n) pos++;
                else if (c == 'H') pos = 0;
                else if (c == 'F') pos = n;
                else if (c == '3') { escape = 3; continue; }
                else if (c == 'A' || c == 'B') {
                    if (history == sh_history_count) sh_copy(draft, out);
                    if (c == 'A' && history) history--;
                    if (c == 'B' && history < sh_history_count) history++;
                    sh_copy(out, history == sh_history_count ? draft : sh_history[history]);
                    n = pos = (unsigned)ustrlen(out);
                }
            }
        } else if (c == 27) { escape = 1; continue; }
        else if (c == '\n' || c == '\r') break;
        else if (c == 3) { out[0] = 0; n = pos = 0; puts_("^C"); break; }
        else if (c == 4 && n == 0) { result = 0; break; }
        else if (c == 1) pos = 0;
        else if (c == 5) pos = n;
        else if (c == 11) { n = pos; out[n] = 0; }
        else if (c == 21) { n = pos = 0; out[0] = 0; }
        else if ((c == '\b' || c == 127) && pos) {
            for (unsigned i = pos; i <= n; i++) out[i-1] = out[i];
            pos--; n--;
        } else if (c == 4 && pos < n) {
            for (unsigned i = pos; i < n; i++) out[i] = out[i+1];
            n--;
        } else if (c == '\t') {
            if (sh_complete(out, &pos, cap) != 0) puts_("\a");
            n = (unsigned)ustrlen(out);
        } else if ((unsigned char)c >= 32 && c != 127 && n + 1 < cap) {
            for (unsigned i = n + 1; i > pos; i--) out[i] = out[i-1];
            out[pos++] = c; n++;
        }
        sh_redraw(out, oldpos, oldlen, pos);
    }
    if (sys_tcsetmode(0) != 0) sys_exit(1);
    puts_("\n");
    if (result) sh_history_add(out);
    return result;
}
#endif
