/*
 * sort.c - 把輸入的每一行依字母順序排序後輸出
 * 用法:sort [-r] [檔案]     沒有給檔案就讀標準輸入;-r 反過來排
 * 例如:cat /README.txt | sort
 *
 * 這個程式示範使用者端的函式庫(libc.h):事先不知道輸入有多大,
 * 所以用 malloc / realloc 讓緩衝區隨著輸入長大,最後用 printf 輸出。
 */
#include "libc.h"

static int reverse;

static int before(const char *a, const char *b)
{
    int c = strcmp(a, b);
    return reverse ? c > 0 : c < 0;
}

/* 合併排序:穩定,而且最壞情況也是 n log n。tmp 是和 lines 一樣大的暫存空間 */
static void merge_sort(char **lines, char **tmp, size_t n)
{
    if (n < 2)
        return;
    size_t half = n / 2, i = 0, j = half, k = 0;

    merge_sort(lines, tmp, half);
    merge_sort(lines + half, tmp, n - half);
    while (i < half && j < n)
        tmp[k++] = before(lines[j], lines[i]) ? lines[j++] : lines[i++];
    while (i < half)
        tmp[k++] = lines[i++];
    while (j < n)
        tmp[k++] = lines[j++];
    memcpy(lines, tmp, n * sizeof(*lines));
}

int main(int argc, char **argv)
{
    size_t cap = 4096, len = 0, nlines = 0, maxlines = 64;
    char *text = malloc(cap);
    char **lines = malloc(maxlines * sizeof(*lines));
    long fd = 0, n;
    int arg = 1;

    if (argc > arg && strcmp(argv[arg], "-r") == 0) {
        reverse = 1;
        arg++;
    }
    if (argc > arg && (fd = sys_open(argv[arg], O_RDONLY)) < 0) {
        fprintf(2, "sort: cannot open %s\n", argv[arg]);
        return 1;
    }
    /* 把全部的輸入讀進一塊會長大的緩衝區 */
    while (text && (n = sys_read(fd, text + len, cap - len - 1)) > 0) {
        len += (size_t)n;
        if (len + 1 >= cap) {
            char *bigger = realloc(text, cap * 2);
            if (!bigger) {
                free(text);
                text = NULL;
                break;
            }
            text = bigger;
            cap *= 2;
        }
    }
    if (!text || !lines) {
        fprintf(2, "sort: out of memory\n");
        return 1;
    }
    text[len] = 0;

    /* 切成一行一行:把換行改成字串的結尾,記下每一行的開頭 */
    for (char *p = text; p < text + len; ) {
        char *nl = strchr(p, '\n');
        if (nlines == maxlines) {
            char **bigger = realloc(lines, maxlines * 2 * sizeof(*lines));
            if (!bigger) {
                fprintf(2, "sort: out of memory\n");
                return 1;
            }
            lines = bigger;
            maxlines *= 2;
        }
        lines[nlines++] = p;
        if (!nl)
            break;
        *nl = 0;
        p = nl + 1;
    }

    char **tmp = malloc((nlines ? nlines : 1) * sizeof(*tmp));
    if (!tmp) {
        fprintf(2, "sort: out of memory\n");
        return 1;
    }
    merge_sort(lines, tmp, nlines);
    for (size_t i = 0; i < nlines; i++)
        printf("%s\n", lines[i]);
    free(tmp);
    free(lines);
    free(text);
    return 0;
}
