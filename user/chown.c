/*
 * chown.c - 更改檔案或目錄的擁有者
 * 用法:chown 使用者 檔案...     使用者可以是名稱(/etc/passwd 裡的)或數字;群組一併改成該使用者的
 * 只有有特權的行程(root)可以做:否則使用者可以把檔案「送」給別人。
 */
#include "passwd.h"

int main(int argc, char **argv)
{
    struct passwd_entry e;
    int rc = 0;

    if (argc < 3) {
        fprintf(2, "usage: chown USER FILE...\n");
        return 1;
    }
    if (!passwd_find(argv[1], 0, &e)) {
        if (argv[1][0] < '0' || argv[1][0] > '9') {
            fprintf(2, "chown: unknown user: %s\n", argv[1]);
            return 1;
        }
        e.uid = e.gid = atoi(argv[1]);
    }
    for (int i = 2; i < argc; i++) {
        long err = sys_chown(argv[i], e.uid, e.gid);
        if (err != 0) {
            fprintf(2, "chown: cannot change %s (%s)\n", argv[i],
                    err == -E_PERM ? "permission denied" : err == -E_NOENT ? "no such file" : "error");
            rc = 1;
        }
    }
    return rc;
}
