/* 原始 shell 控制流程，描述符表與終端機狀態改用可觀察的主機替身。 */
#include <stdio.h>
_Noreturn void _Exit(int);
#include <string.h>
#include "errno.h"
#include "uapi_proc.h"
#include "shell_types.h"
static int fds[16],foreground,waits,spawns,reaped;
static int syncs,sync_fd=-1,sync_before_wait,keep_closes;
static const char *fault;
static int puts_(const char *s) { (void)s;return 0; }
static long sys_tcsetfg(long pgid) { foreground=(int)pgid;return 0; }
static long sys_kill(long pid,int sig) {
    (void)pid;(void)sig;
    if (!strcmp(fault,"resume-denied")) return -E_PERM;
    return !strcmp(fault,"resume-gone") ? -E_SRCH:0;
}
static long sys_waitpid(long pid,int *status,int flags) {
    (void)flags;waits++;
    if (!strcmp(fault,"wait-always-interrupted")) return -E_INTR;
    if (!strcmp(fault,"wait-io-error")) return -E_IO;
    if (!strcmp(fault,"wait-interrupted") && waits==1) return -E_INTR;
    *status=0;reaped++;return pid;
}
static long sys_dup2(long from,long to) {
    if (!strcmp(fault,"redirect-save-error") && to==SAVE_SYNC) return -E_MFILE;
    if (from<0 || from>=16 || to<0 || to>=16 || fds[from]<0) return -E_BADF;
    fds[to]=fds[from];return to;
}
static long sys_close(long fd) {
    if (fd<0 || fd>=16 || fds[fd]<0) return -E_BADF;
    fds[fd]=-1;return 0;
}
static long sync_close(long fd) {
    long rc=sys_close(fd);
    if (fd==SAVE_SYNC) {
        keep_closes++;
        if (!strcmp(fault,"redirect-close-error")) return -E_IO;
    }
    return rc;
}
static void close_quiet(long fd) { if (fd>=0) (void)sync_close(fd); }
static long sys_fsync(long fd) {
    syncs++;sync_fd=(int)fd;sync_before_wait|=reaped==0;
    if (!strcmp(fault,"redirect-sync-error")) return -E_IO;
    if (!strcmp(fault,"redirect-device")) return -E_INVAL;
    return 0;
}
static long sys_open(const char *path,int flags) {
    (void)path;(void)flags;
    for (int i=0;i<16;i++) if (fds[i]<0) { fds[i]=100+i;return i; }
    return -E_MFILE;
}
static long sys_pipe(int out[2]) {
    out[0]=(int)sys_open("pipe",0);out[1]=(int)sys_open("pipe",0);
    return out[0]<0 || out[1]<0 ? -E_MFILE:0;
}
static long sys_spawn(const char *path,char **argv) { (void)path;(void)argv;spawns++;return 101; }
static long sys_setpgid(long pid,long pgid) { (void)pid;(void)pgid;return 0; }
_Noreturn void sys_exit(int status) { _Exit(status); }
static struct job jobs[MAX_JOBS];
static struct job *job_free_slot(void) { return jobs; }
static void job_describe(struct job *j,struct stage *st,int ns) { (void)j;(void)st;(void)ns; }
static void job_report(struct job *j,const char *text,long pid) { (void)j;(void)text;(void)pid; }
static void job_report_stopped(struct job *j) { (void)j; }
static void complain(const char *a,const char *b) { (void)a;(void)b; }
#define sys_close sync_close
#include "shell_functions.c"
#undef sys_close
static int open_count(void) { int n=0;for (int i=0;i<16;i++) n+=fds[i]>=0;return n; }
int main(int argc,char **argv) {
    if (argc!=2) return 90;
    fault=argv[1];for (int i=0;i<16;i++) fds[i]=-1;
    fds[0]=10;fds[1]=11;fds[2]=12;
    long rc;int errors=0,left=-1;long remaining_pid=-1;
    if (!strcmp(fault,"resume-denied") || !strncmp(fault,"wait-",5) || !strcmp(fault,"resume-gone")) {
        struct job j={.used=1,.pgid=99,.n=1,.left=1,.pids={101},.stopped=SIGSTOP};
        rc=job_wait(&j);
        left=j.left;remaining_pid=j.pids[0];
        if (!strcmp(fault,"resume-denied")) errors=rc!=1 || j.stopped!=SIGSTOP || waits!=0 || foreground!=0;
        else if (!strcmp(fault,"wait-always-interrupted") || !strcmp(fault,"wait-io-error"))
            errors=rc==0 || j.left!=1 || j.pids[0]!=101 || reaped!=0 || foreground!=0 || waits>1000;
        else if (!strcmp(fault,"wait-interrupted")) errors=rc!=0 || waits!=2 || reaped!=1 || foreground!=0 || j.status!=0;
        else errors=rc!=0 || waits!=1 || foreground!=0;
    } else {
        if (!strcmp(fault,"closed-stdout")) fds[1]=-1;
        if (!strcmp(fault,"closed-stdin")) fds[0]=-1;
        int before=open_count(),before_fds[16];
        memcpy(before_fds,fds,sizeof(fds));
        int redirect=!strncmp(fault,"redirect-",9);
        struct stage st={.argc=1,.argv={"echo",NULL},.out_path=redirect?"/disk/out":NULL};
        int background=!strcmp(fault,"redirect-background");
        rc=run(&st,1,background);
        int failed=!redirect && strcmp(fault,"run-normal")!=0;
        errors=open_count()!=before || memcmp(before_fds,fds,sizeof(fds)) || (failed && (spawns || rc==0));
        if (!failed && !redirect) errors|=rc!=0 || spawns!=1 || foreground!=0;
        if (redirect) {
            int save_error=!strcmp(fault,"redirect-save-error");
            int output_error=!strcmp(fault,"redirect-sync-error") || !strcmp(fault,"redirect-close-error");
            if (save_error) errors|=spawns!=0 || rc==0;
            else {
                errors|=spawns!=1 || rc!=(output_error?1:0) || foreground!=0 || keep_closes!=1;
                errors|=background?syncs!=0:(syncs!=1 || sync_fd!=SAVE_SYNC || sync_before_wait);
            }
        }
    }
    printf("{\"rc\":%ld,\"foreground\":%d,\"wait_calls\":%d,\"spawn_calls\":%d,"
           "\"reaped\":%d,\"open_fds\":%d,\"save_in_open\":%d,\"save_out_open\":%d,"
           "\"job_left\":%d,\"remaining_pid\":%ld}\n",
           rc,foreground,waits,spawns,reaped,open_count(),fds[SAVE_IN]>=0,fds[SAVE_OUT]>=0,left,remaining_pid);
    return errors;
}
