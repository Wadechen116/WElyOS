/* 真實 run/job_wait 的描述符矩陣;物件身分可分辨主控台、檔案與每條 pipe 的兩端。 */
#include <stdio.h>
int atoi(const char *);
_Noreturn void abort(void);
#include <string.h>
#include <setjmp.h>
#include "errno.h"
#include "fs.h"
#include "proc.h"
#include "shell_types.h"
enum { DUP, OPEN, PIPE, SPAWN, SYNC, CLOSE, OPS };
static const char *op_names[]={"dup2","open","pipe","spawn","fsync","close"};
static int fds[16],counts[OPS],target=-1,nth,forced_open,ns,redirects;
static int pipe_in[MAX_STAGES],pipe_out[MAX_STAGES],made_pipes;
static int spawned[MAX_STAGES],reaped[MAX_STAGES],bad_fds,bad_wait,bad_sync;
static int foreground,exited,exit_status,save_failed,keep_close_failed;
static jmp_buf exit_point;
static struct job jobs[MAX_JOBS];
static int fail(int op) {
    counts[op]++;
    return (target==op && counts[op]==nth) || (op==OPEN && counts[op]==forced_open);
}
static int allocate(int object) {
    for (int i=0;i<16;i++) if (!fds[i]) { fds[i]=object;return i; }
    return -E_MFILE;
}
static long sys_dup2(long from,long to) {
    if (fail(DUP)) {
        if (to==SAVE_SYNC) save_failed=1;
        return -E_MFILE;
    }
    if (from<0 || from>=16 || to<0 || to>=16 || !fds[from]) return -E_BADF;
    fds[to]=fds[from];return to;
}
static long sys_close(long fd) {
    int error=fail(CLOSE);
    if (fd<0 || fd>=16 || !fds[fd]) return -E_BADF;
    if (error && fd==SAVE_SYNC) keep_close_failed=1;
    fds[fd]=0;return error?-E_IO:0;
}
static void close_quiet(long fd) { if (fd>=0) (void)sys_close(fd); }
static long sys_open(const char *path,int flags) {
    (void)flags;
    if (fail(OPEN)) return -E_ACCES;
    return allocate(!strcmp(path,"input")?200:201);
}
static long sys_pipe(int out[2]) {
    if (fail(PIPE)) return -E_MFILE;
    int i=made_pipes++;
    pipe_in[i]=100+i*2;pipe_out[i]=101+i*2;
    out[0]=allocate(pipe_in[i]);out[1]=allocate(pipe_out[i]);
    if (out[0]<0 || out[1]<0) abort();
    return 0;
}
static long sys_spawn(const char *path,char **argv) {
    (void)path;int i=atoi(argv[0]);
    int input=i?pipe_in[i-1]:((redirects&1)?200:1);
    int output=i+1<ns?pipe_out[i]:((redirects&2)?201:2);
    if (fds[0]!=input || fds[1]!=output) bad_fds++;
    if (fail(SPAWN)) return -E_NOENT;
    spawned[i]=1;return 1000+i;
}
static long sys_waitpid(long pid,int *status,int flags) {
    (void)flags;int i=(int)pid-1000;
    if (i<0 || i>=ns || !spawned[i] || reaped[i]) { bad_wait++;return -E_CHILD; }
    reaped[i]=1;*status=0;return pid;
}
static long sys_fsync(long fd) {
    if (fd!=SAVE_SYNC || fds[fd]!=201) bad_sync++;
    for (int i=0;i<ns;i++) if (spawned[i] && !reaped[i]) bad_sync++;
    return fail(SYNC)?-E_IO:0;
}
static long sys_tcsetfg(long pgid) { foreground=(int)pgid;return 0; }
static long sys_kill(long pid,int sig) { (void)pid;(void)sig;return 0; }
static long sys_setpgid(long pid,long pgid) { (void)pid;(void)pgid;return 0; }
_Noreturn void sys_exit(int status) { exited=1;exit_status=status;longjmp(exit_point,1); }
static void puts_(const char *s) { (void)s; }
static void complain(const char *a,const char *b) { (void)a;(void)b; }
static struct job *job_free_slot(void) { return jobs; }
static void job_describe(struct job *j,struct stage *st,int n) { (void)j;(void)st;(void)n; }
static void job_report(struct job *j,const char *s,long pid) { (void)j;(void)s;(void)pid; }
static void job_report_stopped(struct job *j) { (void)j; }
#include "shell_functions.c"
int main(int argc,char **argv) {
    if (argc!=6) return 90;
    ns=atoi(argv[1]);redirects=atoi(argv[2]);nth=atoi(argv[4]);forced_open=atoi(argv[5]);
    if (ns<1 || ns>3 || ns>MAX_STAGES || redirects<0 || redirects>3) return 90;
    for (int i=0;i<OPS;i++) if (!strcmp(argv[3],op_names[i])) target=i;
    fds[0]=1;fds[1]=2;fds[2]=3;
    struct stage st[MAX_STAGES]={0};char programs[MAX_STAGES][16];
    for (int i=0;i<ns;i++) {
        snprintf(programs[i],sizeof(programs[i]),"%d",i);
        st[i].argc=1;st[i].argv[0]=programs[i];
    }
    if (redirects&1) st[0].in_path="input";
    if (redirects&2) st[ns-1].out_path="output";
    volatile long rc=-99;
    if (!setjmp(exit_point)) rc=run(st,ns,0);
    int leaked=0,unreaped=0;
    if (!exited) {
        if (fds[0]!=1 || fds[1]!=2 || fds[2]!=3) bad_fds++;
        for (int i=3;i<16;i++) leaked+=fds[i]!=0;
        for (int i=0;i<ns;i++) unreaped+=spawned[i] && !reaped[i];
    }
    int skipped_sync=save_failed && spawned[ns-1];
    int errors=bad_fds || bad_wait || bad_sync || leaked || unreaped || skipped_sync || (!exited && foreground);
    if (target<0 && !forced_open && (rc!=0 || exited)) errors=1;
    if (target==SYNC && counts[SYNC]>=nth && !exited && rc==0) errors=1;
    if (keep_close_failed && !exited && rc==0) errors=1;
    printf("{\"rc\":%ld,\"exited\":%d,\"exit_status\":%d,\"bad_fds\":%d,\"leaked\":%d,\"unreaped\":%d,\"skipped_sync\":%d,\"counts\":{",
           rc,exited,exit_status,bad_fds,leaked,unreaped,skipped_sync);
    for (int i=0;i<OPS;i++) printf("%s\"%s\":%d",i?",":"",op_names[i],counts[i]);
    puts("}}");return errors;
}
