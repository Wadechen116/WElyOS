/*
 * chmod.c - 更改檔案或目錄的權限
 * 用法:chmod 權限 檔案...       權限是三位八進位數字,依序是擁有者、群組、其他人;
 *                                每一位是 讀 4 + 寫 2 + 執行 1
 * 例如:chmod 755 /disk/prog    擁有者可讀寫執行,其他人可讀與執行
 *       chmod 600 notes.txt     只有擁有者可以讀寫
 * 只有檔案的擁有者(或有特權的行程)可以改。
 */
#include "libc.h"

int main(int argc, char **argv)
{
    int rc = 0;
    long mode = 0;

    if (argc < 3) {
        fprintf(2, "usage: chmod MODE FILE...   (MODE is octal, e.g. 644 or 755)\n");
        return 1;
    }
    for (const char *c = argv[1]; *c; c++) {
        if (*c < '0' || *c > '7' || c - argv[1] >= 4) {
            fprintf(2, "chmod: invalid mode: %s\n", argv[1]);
            return 1;
        }
        mode = mode * 8 + (*c - '0');
    }
    for (int i = 2; i < argc; i++) {
        long err = sys_chmod(argv[i], mode);
        if (err != 0) {
            fprintf(2, "chmod: cannot change %s (%s)\n", argv[i],
                    err == -E_PERM ? "not the owner" : err == -E_NOENT ? "no such file" : "error");
            rc = 1;
        }
    }
    return rc;
}
