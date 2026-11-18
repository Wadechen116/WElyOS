/* 主機故障注入替身：不呼叫 WElyOS 的系統呼叫。 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <stdio.h>
#include <errno.h>
#include <dirent.h>
#include <stdint.h>
#include "welyos_errno.h"
static const char *fault;
static int at, calls, child, replica, forks, allocations;
static pid_t children[4];
static void *owned[3];
static int hit(const char *name) {
    return !strcmp(fault, name) && ++calls == at;
}
static int sys_pipe(int fd[2]) {
    if (hit("pipe")) return -E_MFILE;
    return pipe(fd);
}
static void *probe_malloc(size_t size) {
    if (hit("malloc")) return NULL;
    void *p = malloc(size);
    if (allocations < 3) owned[allocations++] = p;
    return p;
}
static long sys_fork(void) {
    ++forks;
    if (hit("fork")) return -E_NOMEM;
    pid_t p = fork();
    if (!p) { child = 1; replica = forks; }
    else if (p > 0) children[forks-1] = p;
    return p;
}
static long sys_read(int fd, void *buf, size_t size) {
    if (!child && hit("parent-read")) return -E_IO;
    if (child && replica == 1 && hit("child-read")) return -E_IO;
    return read(fd, buf, size);
}
static long sys_write(int fd, const void *buf, size_t size) {
    if (!child && hit("parent-write")) return -1;
    if (!child && hit("short-write")) return write(fd, buf, 1);
    if (child && replica == 1 && hit("child-write")) return -1;
    return write(fd, buf, size);
}
static long sys_dup2(int from, int to) {
    if (child && replica == 1 && hit("dup2")) return -1;
    return dup2(from, to);
}
static long sys_waitpid(long pid, int *status, int flags) {
    if (hit("waitpid")) return -E_INTR;
    int raw;
    pid_t result;
    do { result = waitpid(pid, &raw, flags); } while (result < 0 && errno == EINTR);
    if (result > 0) *status = WIFEXITED(raw) ? WEXITSTATUS(raw) : 128 + WTERMSIG(raw);
    return result;
}
static int fd_count(void) {
    DIR *d = opendir("/proc/self/fd");
    if (!d) _exit(90);
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) if (strcmp(e->d_name,".") && strcmp(e->d_name,"..")) ++n;
    closedir(d); return n;
}
#define sys_close close
#define sys_kill kill
#define sys_exit _exit
static void sys_sleep(long ms) { usleep(ms * 1000); }
#define malloc probe_malloc
