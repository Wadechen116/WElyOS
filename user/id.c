/*
 * id.c - 顯示自己的身分與特權
 * 用法:id
 * 輸出例:uid=0(root) gid=0 caps=fs,kill,setid,admin
 */
#include "passwd.h"

int main(void)
{
    static const char *const names[] = { "fs", "kill", "setid", "admin" };
    char name[32];
    long caps = sys_capget();
    int first = 1;

    printf("uid=%ld(%s) gid=%ld caps=", sys_getuid(), passwd_name(sys_getuid(), name, sizeof(name)),
           sys_getgid());
    for (int i = 0; i < 4; i++) {
        if (caps & (1 << i)) {
            printf("%s%s", first ? "" : ",", names[i]);
            first = 0;
        }
    }
    printf("%s\n", first ? "none" : "");
    return 0;
}
