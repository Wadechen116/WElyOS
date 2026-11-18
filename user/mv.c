/*
 * mv.c - 改名或搬移檔案與目錄
 * 用法:mv 來源 目的
 *   目的是一個已經存在的目錄時,搬進那個目錄裡(名稱不變):mv a.txt /disk/notes
 *   否則就是改成那個名稱:mv a.txt b.txt
 * 只能在同一個檔案系統內搬(/ 與 /disk 之間要用 cp 再 rm);目的檔已經存在時不會蓋掉它。
 */
#include "ulib.h"

int main(int argc, char **argv)
{
    char dest[256];
    struct stat st;
    unsigned n = 0;

    if (argc != 3) {
        puts_("usage: mv SOURCE DEST\n");
        return 1;
    }
    for (const char *c = argv[2]; *c && n < sizeof(dest) - 1; c++)
        dest[n++] = *c;
    dest[n] = 0;
    if (sys_stat(argv[2], &st) == 0 && st.type == I_DIR) {
        /* 目的是目錄:接上來源的最後一段名稱 */
        const char *base = argv[1];
        for (const char *c = argv[1]; *c; c++)
            if (*c == '/' && c[1])
                base = c + 1;
        if (n && dest[n - 1] != '/' && n < sizeof(dest) - 1)
            dest[n++] = '/';
        for (const char *c = base; *c && *c != '/' && n < sizeof(dest) - 1; c++)
            dest[n++] = *c;
        dest[n] = 0;
    }
    long rc = sys_rename(argv[1], dest);
    if (rc != 0) {
        puts_("mv: cannot move ");
        puts_(argv[1]);
        puts_(rc == -E_EXIST ? " (destination exists)\n" :
              rc == -E_XDEV ? " (different file system; use cp and rm)\n" :
              rc == -E_NOENT ? " (no such file or directory)\n" :
              rc == -E_INVAL ? " (cannot move a directory into itself)\n" : "\n");
        return 1;
    }
    return 0;
}
