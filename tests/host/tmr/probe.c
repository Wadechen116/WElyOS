/* 每個案例獨立行程，統計 tmr_run 回傳時的資源與投票結果。 */
#include "tmr.h"
#undef malloc
static int work(void *arg) {
    (void)arg;
    if (!strcmp(fault,"early-close")) { close(1); for (;;) pause(); }
    if (!strcmp(fault,"timer-drain")) {
        char b[256]; memset(b,'x',sizeof(b));
        for (;;) { if (sys_write(1,b,sizeof(b)) < 0) return 2; }
    }
    if (!strcmp(fault,"overflow")) {
        char b[256]; memset(b,'x',sizeof(b));
        for (int i=0;i<33;i++) if (sys_write(1,b,sizeof(b)) != sizeof(b)) return 2;
        return 0;
    }
    /* read-failure 案例需相同的空輸出，以檢查錯誤是否被第二個 read 覆蓋。 */
    if (strcmp(fault,"parent-read") && sys_write(1,"ok",2) != 2) return 2;
    return 0;
}
int main(int argc,char **argv) {
    if (argc != 3) return 91;
    fault=argv[1]; at=atoi(argv[2]);
    signal(SIGPIPE,SIG_IGN);
    struct tmr_result r[3]; struct tmr_report rep;
    int before=fd_count();
    int rc=tmr_run(work,NULL,30,NULL,NULL,r,&rep);
    int fd_delta=fd_count()-before, unreaped=0, running=0, ownership_bad=0;
    for (int i=0;i<4;i++) if (children[i]>0) {
        int st; pid_t p=waitpid(children[i],&st,WNOHANG);
        if (p==children[i]) ++unreaped;
        else if (!p) { ++running; kill(children[i],SIGKILL); waitpid(children[i],&st,0); }
    }
    for (int i=0;i<3;i++) {
        int known=r[i].out==NULL;
        for (int j=0;j<allocations;j++) if (r[i].out==owned[j]) known=1;
        if (!known) ownership_bad=1;
    }
    printf("{\"rc\":%d,\"agree\":%d,\"invalid\":[%d,%d,%d],\"lengths\":[%u,%u,%u],"
           "\"fd_delta\":%d,\"unreaped\":%d,\"running\":%d,\"ownership_bad\":%d,\"timed_out\":%d}\n",
           rc,rep.agree,r[0].invalid,r[1].invalid,r[2].invalid,r[0].len,r[1].len,r[2].len,
           fd_delta,unreaped,running,ownership_bad,rep.timed_out);
    for (int i=0;i<3;i++) free(r[i].out);
    return 0;
}
