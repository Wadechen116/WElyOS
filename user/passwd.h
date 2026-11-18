#ifndef WELYOS_PASSWD_H
#define WELYOS_PASSWD_H
/*
 * passwd.h - 使用者名稱與編號的對照(/etc/passwd)
 *
 * kernel 只認得數字(uid、gid)。名稱是給人看的,對照表是一個普通的文字檔,
 * 每行一個使用者:  名稱:uid:gid
 * 這個檔案只有 root 可以改;裡面沒有密碼 — 這個系統沒有「登入」,
 * 身分只能由有特權的行程往下降(見 su.c)。
 */
#include "libc.h"

struct passwd_entry {
    char name[32];
    long uid, gid;
};

/* 依名稱(name 不為 NULL)或編號尋找;找到回傳 1。檔案不存在或找不到回傳 0 */
static inline int passwd_find(const char *name, long uid, struct passwd_entry *out)
{
    static char buf[1024];
    long fd = sys_open("/etc/passwd", O_RDONLY);
    long n = fd >= 0 ? sys_read(fd, buf, sizeof(buf) - 1) : 0;

    if (fd >= 0)
        sys_close(fd);
    if (n <= 0)
        return 0;
    buf[n] = 0;
    for (char *line = buf; *line; ) {
        char *end = strchr(line, '\n');
        char *c1 = strchr(line, ':');
        if (end)
            *end = 0;
        char *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
        if (c1 && c2 && (size_t)(c1 - line) < sizeof(out->name)) {
            memcpy(out->name, line, (unsigned long)(c1 - line));
            out->name[c1 - line] = 0;
            out->uid = atoi(c1 + 1);
            out->gid = atoi(c2 + 1);
            if (name ? strcmp(name, out->name) == 0 : out->uid == uid)
                return 1;
        }
        if (!end)
            break;
        line = end + 1;
    }
    return 0;
}

/* 把 uid 寫成名稱;對照表裡沒有就寫成數字 */
static inline const char *passwd_name(long uid, char *buf, size_t cap)
{
    struct passwd_entry e;

    if (passwd_find(NULL, uid, &e))
        snprintf(buf, cap, "%s", e.name);
    else
        snprintf(buf, cap, "%ld", uid);
    return buf;
}
#endif
