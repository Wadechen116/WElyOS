/* Actual run/reap/wait lifecycle. FDs carry file identities; children write only
 * when completed by wait/reap, so premature sync/close is observable. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "errno.h"
#include "proc.h"
#include "fs.h"
#include "types.h"
static int fds[16], next_file=100, files[64], writes[64], syncs[64], closes[64];
static int bad_sync,fail_sync,fail_close,fail_spawn,spawn_count,next_stop,wait_error,complaints;
static char complained_path[256];
struct child {long pid;int output,done,reported,code;};
static struct child children[64];
static struct job jobs[MAX_JOBS];
static long async_status;
static int streq(const char *a,const char *b) {return !strcmp(a,b);}
static int object(long fd) {return fd>=0&&fd<16?fds[fd]:0;}
static long sys_open(const char *path,int flags) {
    (void)flags;
    int obj=!strcmp(path,"/dev/null")?4:next_file++;
    for(int i=3;i<16;i++)if(!fds[i]){fds[i]=obj;if(obj>=100)files[obj-100]=1;return i;}
    return -E_MFILE;
}
static long sys_close(long fd) {
    int obj=object(fd);
    if(!obj)return -E_BADF;
    fds[fd]=0;
    if(obj>=100 && fd>=3){closes[obj-100]++;if(fail_close==obj || fail_close==-1)return -E_IO;}
    return 0;
}
static void close_quiet(long fd) {if(fd>=0)(void)sys_close(fd);}
static long sys_dup2(long src,long dst) {
    if(!object(src)||dst<0||dst>=16)return -E_BADF;
    fds[dst]=fds[src];return dst;
}
static long sys_pipe(int out[2]) {
    out[0]=(int)sys_open("/dev/null",0);out[1]=(int)sys_open("/dev/null",0);
    if(out[0]<0||out[1]<0){close_quiet(out[0]);close_quiet(out[1]);return -E_MFILE;}return 0;
}
static long sys_fsync(long fd) {
    int obj=object(fd);
    if(obj<100){bad_sync++;return -E_BADF;}
    syncs[obj-100]++;
    for(int i=0;i<spawn_count;i++)if(children[i].output==obj&&!children[i].done)bad_sync++;
    return fail_sync==obj||fail_sync==-1?-E_IO:0;
}
static long sys_spawn(const char *path,char **argv) {
    (void)path;(void)argv;
    if(fail_spawn)return -E_NOENT;
    int i=spawn_count++;assert(i<64);
    children[i]=(struct child){.pid=1000+i,.output=object(1)};return children[i].pid;
}
static void finish_child(int i) {
    assert(i>=0&&i<spawn_count);
    if(!children[i].done&&children[i].output>=100)writes[children[i].output-100]++;
    children[i].done=1;
}
static long sys_waitpid(long pid,int *status,int flags) {
    if(wait_error)return -E_IO;
    if(pid<0){
        for(int i=0;i<spawn_count;i++)if(children[i].done&&!children[i].reported){
            children[i].reported=1;*status=children[i].code;return children[i].pid;
        }
        return 0;
    }
    int i=(int)pid-1000;
    if(i<0||i>=spawn_count||children[i].reported)return -E_CHILD;
    if(next_stop){next_stop=0;*status=(int)(WSTOPPED_BASE|SIGSTOP);return pid;}
    if((flags&WNOHANG)&&!children[i].done)return 0;
    finish_child(i);children[i].reported=1;*status=children[i].code;return pid;
}
static long sys_tcsetfg(long pg) {(void)pg;return 0;}
static long sys_kill(long pid,int sig) {(void)pid;(void)sig;return 0;}
static long sys_setpgid(long pid,long pg) {(void)pid;(void)pg;return 0;}
static void sys_exit(int code) {(void)code;assert(0);}
static void puts_(const char *s) {(void)s;}
static void complain(const char *a,const char *b) {(void)a;complaints++;snprintf(complained_path,sizeof(complained_path),"%s",b);}
static void job_describe(struct job *j,struct stage *st,int n) {(void)j;(void)st;(void)n;}
static void job_report(struct job *j,const char *s,long n) {(void)j;(void)s;(void)n;}
static void job_report_stopped(struct job *j) {(void)j;}
static void job_finish(struct job *j);
#include "functions.c"
static long launch(char *path,int bg) {
    struct stage st={.argc=1,.argv={"echo",NULL},.out_path=path};return run(&st,1,bg);
}
static int open_count(void) {int n=0;for(int i=3;i<16;i++)n+=fds[i]!=0;return n;}
static void check_file(int i,int w,int sy,int cl) {assert(writes[i]==w&&syncs[i]==sy&&closes[i]==cl);}
int main(int argc,char **argv) {
    if(argc!=2)return 90;
    fds[0]=1;fds[1]=2;fds[2]=3;
    const char *test=argv[1];
    if(streq(test,"background")) {
        assert(launch("outA",1)==0);assert(open_count()==1);check_file(0,0,0,0);
        assert(job_wait_all()==0);check_file(0,1,1,1);assert(!open_count());
    } else if(streq(test,"reap-error")) {
        fail_sync=-1;assert(launch("outA",1)==0);finish_child(0);jobs_reap();
        check_file(0,1,1,1);assert(!open_count());assert(async_status==1);
        assert(job_wait_all()==1);assert(job_wait_all()==0);assert(complaints==1);
    } else if(streq(test,"close-error")) {
        fail_close=-1;assert(launch("outA",1)==0);assert(job_wait_all()==1);
        check_file(0,1,1,1);jobs_reap();assert(job_wait_all()==0);check_file(0,1,1,1);
    } else if(streq(test,"two-jobs")) {
        fail_sync=100;assert(launch("outA",1)==0);assert(launch("outB",1)==0);
        assert(jobs[0].save_fd!=jobs[1].save_fd);assert(open_count()==2);
        assert(job_wait_all()==1);check_file(0,1,1,1);check_file(1,1,1,1);assert(!open_count());
    } else if(streq(test,"stop-resume")) {
        next_stop=1;assert(launch("outA",0)==128+SIGSTOP);assert(jobs[0].stopped==SIGSTOP);
        check_file(0,0,0,0);assert(jobs[0].save_pending&&open_count()==1);
        assert(job_wait(&jobs[0])==0);jobs[0].used=0;check_file(0,1,1,1);assert(!open_count());
    } else if(streq(test,"wait-error")) {
        wait_error=1;assert(launch("outA",0)==1);assert(jobs[0].used&&jobs[0].save_pending);
        check_file(0,0,0,0);wait_error=0;assert(job_wait_all()==0);check_file(0,1,1,1);
    } else if(streq(test,"spawn-error")) {
        fail_spawn=1;assert(launch("outA",1)==127);check_file(0,0,1,1);assert(!open_count());
    } else if(streq(test,"full-jobs")) {
        for(int i=0;i<MAX_JOBS;i++)jobs[i].used=1;
        assert(launch("outA",1)==1);assert(!spawn_count&&!open_count());
        assert(launch("outA",0)==1);assert(!spawn_count&&!open_count());
    } else if(streq(test,"full-fds")) {
        for(int i=3;i<14;i++)fds[i]=4;
        assert(launch("outA",1)==1);assert(!spawn_count&&open_count()==11);
    } else if(streq(test,"path-lifetime")) {
        char name[32]="original";fail_sync=-1;assert(launch(name,1)==0);strcpy(name,"overwritten");
        assert(job_wait_all()==1);assert(streq(complained_path,"original"));
    } else if(streq(test,"exit-pending")) {
        assert(launch("outA",1)==0);assert(sh_exit_status(0)==1);assert(sh_exit_status(7)==7);
        assert(job_wait_all()==0);assert(sh_exit_status(0)==0);
    } else if(streq(test,"stopped-wait")) {
        next_stop=1;assert(launch("outA",0)==128+SIGSTOP);
        assert(job_wait_all()==128+SIGSTOP);assert(jobs[0].save_pending);check_file(0,0,0,0);
        assert(job_wait(&jobs[0])==0);jobs[0].used=0;check_file(0,1,1,1);
    } else if(streq(test,"pipe-fd-collision")) {
        for(int i=3;i<13;i++)fds[i]=4;
        struct stage st[2]={{.argc=1,.argv={"a",NULL}},{.argc=1,.argv={"b",NULL}}};
        assert(run(st,2,0)==1);assert(!spawn_count&&open_count()==10);
        assert(fds[0]==1&&fds[1]==2&&!fds[SAVE_IN]&&!fds[SAVE_OUT]);
    } else return 91;
    assert(!bad_sync);
    printf("%s PASS spawns=%d complaints=%d open=%d\n",test,spawn_count,complaints,open_count());return 0;
}
