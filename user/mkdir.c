/*
 * mkdir.c - 建立目錄
 * 用法:mkdir 目錄...   全部成功回傳 0
 */
#include "ulib.h"

int main(int argc, char **argv)
{
    int rc = 0;

    for (int i = 1; i < argc; i++) {
        if (sys_mkdir(argv[i]) != 0) {
            puts_("mkdir: cannot create ");
            puts_(argv[i]);
            puts_("\n");
            rc = 1;
        }
    }
    return rc;
}
