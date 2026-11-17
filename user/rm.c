/*
 * rm.c - 刪除檔案或空的目錄
 * 用法:rm 路徑...   全部成功回傳 0
 */
#include "ulib.h"

int main(int argc, char **argv)
{
    int rc = 0;

    for (int i = 1; i < argc; i++) {
        long err = sys_unlink(argv[i]);
        if (err != 0) {
            puts_("rm: cannot remove ");
            puts_(argv[i]);
            puts_(err == -E_NOTEMPTY ? " (directory not empty)\n" : "\n");
            rc = 1;
        }
    }
    return rc;
}
