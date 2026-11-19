/*
 * wc.c - 計算行數、字數、位元組數
 * 用法:wc [檔案]   沒有給檔案就讀標準輸入(所以可以接在管線後面:ls /bin | wc)
 * 輸出:行數 字數 位元組數
 */
#include "ulib.h"

int main(int argc, char **argv)
{
    char buf[256];
    long fd = 0, n, lines = 0, words = 0, bytes = 0;
    int in_word = 0;

    if (argc > 1) {
        fd = sys_open(argv[1], O_RDONLY);
        if (fd < 0) {
            puts_("wc: cannot open ");
            puts_(argv[1]);
            puts_("\n");
            return 1;
        }
    }
    while ((n = sys_read(fd, buf, sizeof(buf))) > 0) {
        bytes += n;
        for (long i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n')
                lines++;
            if (c == ' ' || c == '\n' || c == '\t') {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                words++;
            }
        }
    }
    struct line l = { .len = 0 };
    line_num(&l, lines);
    line_str(&l, " ");
    line_num(&l, words);
    line_str(&l, " ");
    line_num(&l, bytes);
    line_str(&l, "\n");
    return line_write(&l) == 0 ? 0 : 1;          /* 結果寫不出去就是失敗 */
}
