/*
 * sh_words.h - shell 的變數、分字(認得引號)與有上限的路徑萬用字元展開(docs/ISSUES.md N-005;Codex 第 51 步交付)
 *
 * 不支援:匯出到環境變數(沒有這個 ABI)、命令替換、遞迴的 **。
 * 上限都是明確的(16 個變數、每個值 255 位元組、90 個字、萬用字元最深 16 層 / 最多走訪 4096 個目錄項目):
 * 超過任何一個時整行拒絕,在建立任何行程之前就停下,不會執行被截斷的命令。
 */
#ifndef WELYOS_SH_WORDS_H
#define WELYOS_SH_WORDS_H
#define SH_WORDS (MAX_ARGS * MAX_STAGES)
#define SH_TEXT 256
#define SH_VARS 16
#define SH_OP 1
#define SH_ASSIGN 2
struct sh_var { char name[32], value[SH_TEXT]; };
static struct sh_var sh_vars[SH_VARS];
static char sh_text[SH_WORDS][SH_TEXT];
static unsigned char sh_mask[SH_WORDS][SH_TEXT], sh_kind[SH_WORDS];
static char sh_paths[SH_WORDS][SH_TEXT];
static unsigned sh_path_count, sh_walks;

static int sh_name_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static int sh_name_char(char c)
{
    return sh_name_start(c) || (c >= '0' && c <= '9');
}
static const char *sh_value(const char *name)
{
    for (int i = 0; i < SH_VARS; i++)
        if (streq(sh_vars[i].name, name))
            return sh_vars[i].value;
    return "";
}
/* 回傳 1 = 是賦值並已設定;0 = 不是賦值;-1 = 名稱或值太長、變數表滿了(不會只改一半) */
static int sh_assign(const char *word)
{
    unsigned n = 0;
    if (!sh_name_start(word[0]))
        return 0;
    while (sh_name_char(word[n]))
        n++;
    if (word[n] != '=')
        return 0;
    if (n >= sizeof(sh_vars[0].name) || ustrlen(word + n + 1) >= SH_TEXT)
        return -1;
    char name[32];
    for (unsigned j = 0; j < n; j++)
        name[j] = word[j];
    name[n] = 0;
    int slot = -1;
    for (int i = 0; i < SH_VARS; i++) {
        if (streq(sh_vars[i].name, name)) { slot = i; break; }
        if (slot < 0 && !sh_vars[i].name[0]) slot = i;
    }
    if (slot < 0)
        return -1;
    for (unsigned j = 0; j <= n; j++)
        sh_vars[slot].name[j] = name[j];
    const char *v = word + n + 1;
    unsigned j = 0;
    do { sh_vars[slot].value[j] = v[j]; } while (v[j++]);
    return 1;
}
static int sh_put(int w, unsigned *len, char c, int wild)
{
    if (*len + 1 >= SH_TEXT)
        return -1;
    sh_text[w][*len] = c;
    sh_mask[w][(*len)++] = (unsigned char)(wild && (c == '*' || c == '?'));
    sh_text[w][*len] = 0;
    return 0;
}
/* 分字:沒有引號的變數值遇到空白就拆成多個字;展開出來的內容永遠不會變成運算子(| < > &)。
   賦值的右邊即使沒有引號也是一整個值。 */
static int sh_lex(const char *s, char **words, long status)
{
    int n = 0;
    for (;;) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#') break;
        if (n >= SH_WORDS) return -1;
        unsigned len = 0;
        int quote = 0, keep = 0, assignment = 0;
        sh_text[n][0] = 0; sh_kind[n] = 0;
        if (*s == '|' || *s == '<' || *s == '>' || *s == '&') {
            char c = *s++;
            if (sh_put(n, &len, c, 0)) return -1;
            if (c == '>' && *s == '>') {
                s++;
                if (sh_put(n, &len, '>', 0)) return -1;
            }
            sh_kind[n] = SH_OP;
        } else {
            /* Only a literal NAME= prefix is an assignment, not "$X". */
            const char *p = s;
            if (sh_name_start(*p)) {
                while (sh_name_char(*p)) p++;
                assignment = *p == '=';
            }
            if (assignment) sh_kind[n] = SH_ASSIGN;
            while (*s) {
                char c = *s;
                if (!quote && (c == ' ' || c == '\t' || c == '|' || c == '<' || c == '>' || c == '&')) break;
                s++;
                if ((c == '\'' || c == '"') && (!quote || quote == c)) {
                    quote = quote ? 0 : c; keep = 1; continue;
                }
                if (c == '\\' && quote != '\'') {
                    if (!*s) return -1;
                    if (quote == '"' && *s != '$' && *s != '"' && *s != '\\') {
                        if (sh_put(n, &len, c, 0)) return -1;
                        continue;
                    }
                    if (sh_put(n, &len, *s++, 0)) return -1;
                    keep = 1; continue;
                }
                if (c == '$' && quote != '\'') {
                    char name[32], num[24]; unsigned k = 0;
                    const char *v;
                    if (*s == '?') {
                        s++;
                        unsigned long u = status < 0 ? 0UL - (unsigned long)status : (unsigned long)status;
                        unsigned d = 0;
                        do { num[d++] = (char)('0' + u % 10); u /= 10; } while (u);
                        if (status < 0) num[d++] = '-';
                        for (unsigned a = 0; a < d / 2; a++) { char t = num[a]; num[a] = num[d-1-a]; num[d-1-a] = t; }
                        num[d] = 0; v = num;
                    } else {
                        int brace = *s == '{';
                        if (brace) s++;
                        if (!sh_name_start(*s)) {
                            if (brace) return -1;
                            if (sh_put(n, &len, '$', 0)) return -1;
                            keep = 1; continue;
                        }
                        while (sh_name_char(*s)) {
                            if (k + 1 >= sizeof(name)) return -1;
                            name[k++] = *s++;
                        }
                        if (brace && *s++ != '}') return -1;
                        name[k] = 0; v = sh_value(name);
                    }
                    while (*v) {
                        if (!quote && !assignment && (*v == ' ' || *v == '\t')) {
                            if (len || keep) {
                                words[n] = sh_text[n]; n++;
                                if (n >= SH_WORDS) return -1;
                                len = 0; keep = 0; sh_kind[n] = 0; sh_text[n][0] = 0;
                            }
                            v++; continue;
                        }
                        if (sh_put(n, &len, *v++, !quote && !assignment)) return -1;
                    }
                    continue;
                }
                keep = 1;
                if (sh_put(n, &len, c, !quote && !assignment)) return -1;
            }
            if (quote) return -1;
            if (!len && !keep) continue;
        }
        words[n] = sh_text[n]; n++;
    }
    words[n] = 0;
    return n;
}
static int sh_operator(const char *word, const char *op)
{
    for (int i = 0; i < SH_WORDS; i++)
        if (word == sh_text[i])
            return (sh_kind[i] & SH_OP) && streq(word, op);
    return 0;
}
/* 萬用字元比對(迴圈,不遞迴):被引號或反斜線保護的 * 與 ? 在 mask 中為 0,當成普通字元 */
static int sh_match(const char *p, const unsigned char *mask, unsigned plen, const char *s)
{
    unsigned i = 0, j = 0, star = plen, retry = 0;
    while (s[j]) {
        if (i < plen && mask[i] && p[i] == '*') { star = i++; retry = j; }
        else if (i < plen && ((mask[i] && p[i] == '?') || p[i] == s[j])) { i++; j++; }
        else if (star < plen) { i = star + 1; j = ++retry; }
        else return 0;
    }
    while (i < plen && mask[i] && p[i] == '*') i++;
    return i == plen;
}
static int sh_path_add(const char *s)
{
    if (sh_path_count >= SH_WORDS || ustrlen(s) >= SH_TEXT) return -1;
    unsigned n = 0;
    do { sh_paths[sh_path_count][n] = s[n]; } while (s[n++]);
    sh_path_count++;
    return 0;
}
/* 走訪目錄:遞迴深度受路徑層數(16)限制,總走訪次數(4096)與符合的數量(90)也有上限 */
static int sh_glob_walk(const char *pattern, const unsigned char *mask,
                        const char *prefix, unsigned depth)
{
    if (depth >= 16 || ++sh_walks > 4096) return -1;
    unsigned plen = 0;
    while (pattern[plen] && pattern[plen] != '/') plen++;
    int wild = 0;
    for (unsigned i = 0; i < plen; i++) wild |= mask[i];
    if (!wild) {
        char path[SH_TEXT]; unsigned n = 0;
        for (const char *p = prefix; *p; p++) { if (n + 1 >= sizeof(path)) return -1; path[n++] = *p; }
        for (unsigned i = 0; i < plen; i++) { if (n + 1 >= sizeof(path)) return -1; path[n++] = pattern[i]; }
        path[n] = 0;
        if (pattern[plen] == '/') {
            if (n + 1 >= sizeof(path)) return -1;
            path[n++] = '/'; path[n] = 0;
            return sh_glob_walk(pattern + plen + 1, mask + plen + 1, path, depth + 1);
        }
        struct stat st;
        long rc = sys_stat(path, &st);
        if (rc == -E_NOENT || rc == -E_NOTDIR) return 0;
        if (rc != 0) return -1;
        return sh_path_add(path);
    }
    long fd = sys_open(*prefix ? prefix : ".", O_RDONLY);
    if (fd == -E_NOENT || fd == -E_NOTDIR) return 0;
    if (fd < 0) return -1;
    struct dirent e; int err = 0;
    for (;;) {
        long rc = sys_readdir(fd, &e);
        if (rc == 1) break;
        if (rc != 0) { err = -1; break; }
        if ((e.name[0] == '.' && pattern[0] != '.') || streq(e.name, ".") || streq(e.name, "..")) continue;
        if (!sh_match(pattern, mask, plen, e.name)) continue;
        char path[SH_TEXT]; unsigned n = 0;
        for (const char *p = prefix; *p; p++) { if (n + 1 >= sizeof(path)) { err = -1; break; } path[n++] = *p; }
        for (const char *p = e.name; !err && *p; p++) { if (n + 1 >= sizeof(path)) { err = -1; break; } path[n++] = *p; }
        if (err) break;
        path[n] = 0;
        if (pattern[plen] == '/') {
            if (e.type != I_DIR) continue;
            if (n + 1 >= sizeof(path)) { err = -1; break; }
            path[n++] = '/'; path[n] = 0;
            err = sh_glob_walk(pattern + plen + 1, mask + plen + 1, path, depth + 1);
        } else err = sh_path_add(path);
        if (err) break;
    }
    long closed = sys_close(fd);
    return err || closed != 0 ? -1 : 0;
}
static int sh_compare(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
/* 展開到固定的儲存區;運算子只會來自分字階段,展開的結果不會變成運算子 */
static int sh_expand(char **words, int n)
{
    char *expanded[SH_WORDS + 1]; int count = 0;
    sh_path_count = 0; sh_walks = 0;
    for (int w = 0; w < n; w++) {
        int wild = 0;
        for (unsigned j = 0; words[w][j]; j++) wild |= sh_mask[w][j];
        unsigned first = sh_path_count;
        if (wild && !(sh_kind[w] & (SH_OP | SH_ASSIGN))) {
            if (sh_glob_walk(words[w], sh_mask[w], "", 0)) return -1;
            /* 重新導向必須只對應一個檔案:符合多個時拒絕,不悄悄丟掉其他的 */
            if (w && (sh_operator(words[w-1], ">") || sh_operator(words[w-1], ">>") || sh_operator(words[w-1], "<")) && sh_path_count - first > 1) return -1;
            for (unsigned i = first + 1; i < sh_path_count; i++)
                for (unsigned j = i; j > first && sh_compare(sh_paths[j-1], sh_paths[j]) > 0; j--) {
                    char tmp[SH_TEXT];
                    for (unsigned k = 0; k < SH_TEXT; k++) { tmp[k] = sh_paths[j-1][k]; sh_paths[j-1][k] = sh_paths[j][k]; sh_paths[j][k] = tmp[k]; }
                }
        }
        if (sh_path_count == first) {
            if (count >= SH_WORDS) return -1;
            expanded[count++] = words[w];
        } else {
            for (unsigned j = first; j < sh_path_count; j++) {
                if (count >= SH_WORDS) return -1;
                expanded[count++] = sh_paths[j];
            }
        }
    }
    for (int i = 0; i < count; i++) words[i] = expanded[i];
    words[count] = 0;
    return count;
}
#endif
