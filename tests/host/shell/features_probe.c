/* Real shell word/editor code against a POSIX directory and deterministic keys.
 * No fake glob results: names are read from files made by the Python driver. */
#define _POSIX_C_SOURCE 200809L
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#undef O_RDONLY
#undef O_WRONLY
#undef O_RDWR
#undef O_ACCMODE
#undef O_CREAT
#undef O_TRUNC
#undef O_APPEND
#undef WIFSTOPPED
#undef WSTOPSIG
#define stat sh_stat
#define dirent sh_dirent
#include "fs.h"
#include "proc.h"
#undef stat
#undef dirent
#define MAX_ARGS 15
#define MAX_STAGES 6
static unsigned long ustrlen(const char *s) { return strlen(s); }
static int streq(const char *a, const char *b) { return strcmp(a,b)==0; }
static DIR *dirs[1024];
static int open_count, close_count, read_error, mode_raw, mode_resets;
static const unsigned char *keys;
static const char *script_data;
static long sys_open(const char *s, int flags) {
    (void)flags; if(!strcmp(s,"@script"))return 900; int fd=open(s,O_RDONLY);
    if(fd<0) return errno==ENOENT?-2:errno==ENOTDIR?-20:-5;
    open_count++; return fd;
}
static long sys_close(long fd) {
    close_count++;
    if(fd<0 || fd>=1024) abort();
    if(dirs[fd]) { closedir(dirs[fd]); dirs[fd]=0; }
    return close((int)fd)==0?0:-5;
}
static long sys_readdir(long fd,struct sh_dirent *e) {
    if(read_error) return -5;
    if(fd<0 || fd>=1024) abort();
    if(!dirs[fd]) { dirs[fd]=fdopendir(dup((int)fd)); if(!dirs[fd]) return -20; }
    errno=0; struct dirent *d=readdir(dirs[fd]);
    if(!d) return errno?-5:1;
    memset(e,0,sizeof(*e));
    if(strlen(d->d_name)>=sizeof(e->name)) return -5;
    strcpy(e->name,d->d_name);
    struct stat st;
    if(fstatat((int)fd,d->d_name,&st,0)!=0) return -5;
    e->type=S_ISDIR(st.st_mode)?I_DIR:I_FILE;
    return 0;
}
static long sys_stat(const char *path,struct sh_stat *out) {
    struct stat st;
    if(stat(path,&st)!=0) return errno==ENOENT?-2:errno==ENOTDIR?-20:-5;
    memset(out,0,sizeof(*out));out->type=S_ISDIR(st.st_mode)?I_DIR:I_FILE;return 0;
}
static long sys_tcsetmode(long mode) { mode_raw=mode!=0;mode_resets+=mode==0;return 0; }
static long sys_read(long fd,void *out,unsigned long n) {
    if(fd==900) {
        unsigned long len=strlen(script_data);if(len>n)len=n;
        memcpy(out,script_data,len);script_data+=len;return (long)len;
    }
    if(!n || !*keys) return 0;
    *(char*)out=(char)*keys++;return 1;
}
static void puts_(const char *s) { (void)s; }
static void sys_exit(int n) { fprintf(stderr,"unexpected shell exit %d\n",n);exit(99); }
struct reader { long fd; char buf[256];long pos,len; };
#include "reader.c"
#define stat sh_stat
#define dirent sh_dirent
#include "errno.h"
#include "words.c"
#include "edit.c"
struct stage { char *argv[MAX_ARGS+1]; int argc;const char *in_path,*out_path;int append; };
#include "parse.c"
#define MAX_JOBS 8
struct job { int used,stopped;long status,pgid; };
static struct job jobs[MAX_JOBS];
static void jobs_reap(void) { }
static struct job *job_pick(const char *s,int stopped) { (void)s;(void)stopped;return 0; }
static void job_report_stopped(struct job *j) { (void)j; }
static void job_report(struct job *j,const char *s,long n) { (void)j;(void)s;(void)n; }
static int job_wait(struct job *j) { (void)j;return 0; }
/* Job persistence is exercised by jobs_probe.c, not this words/editor fixture. */
static inline long job_wait_all(void) { return 0; }
static inline int sh_exit_status(long status) { return (int)status; }
static long sys_getuid(void) { return 0; }
static long sys_getcwd(char *out,unsigned long n) { (void)n;strcpy(out,"/");return 1; }
static long sys_chdir(const char *s) { return strcmp(s,"missing") ? 0 : -2; }
static long sys_memlimit(long n) { return n; }
static long sys_kill(long p,int sig) { (void)p;(void)sig;return 0; }
static long uatoi(const char *s) { return strtol(s,0,10); }
static void print_num(const char *a,long n,const char *b) { (void)a;(void)n;(void)b; }
static void complain(const char *a,const char *b) { (void)a;(void)b; }
static int run_count, run_argc[16];
static char run_args[16][MAX_ARGS][SH_TEXT];
static long run(struct stage *st,int ns,int bg) {
    (void)bg;
    for(int i=0;i<ns;i++) {
        if(run_count>=16)abort();
        run_argc[run_count]=st[i].argc;
        for(int j=0;j<st[i].argc;j++)strcpy(run_args[run_count][j],st[i].argv[j]);
        run_count++;
    }
    return st[ns-1].argc;
}
#include "shell_main.c"
static int failures;
#define CHECK(c) do { if(!(c)) { fprintf(stderr,"line %d failed: %s\n",__LINE__,#c); failures++; } } while(0)
static char *words[SH_WORDS+1];
static int lex(const char *s,long status) { return sh_lex(s,words,status); }
static void expect(const char *s,long status,int argc,const char **args) {
    int n=lex(s,status);CHECK(n>=0);if(n<0)return;
    n=sh_expand(words,n);CHECK(n==argc);
    for(int i=0;i<n && i<argc;i++)CHECK(streq(words[i],args[i]));
}
static void edit(const char *input,const char *want,int result) {
    keys=(const unsigned char*)input;char out[SH_TEXT];struct reader r={0};int resets=mode_resets;
    CHECK(sh_read_edit(&r,out,sizeof(out))==result);CHECK(streq(out,want));CHECK(!mode_raw);CHECK(mode_resets==resets+1);
}
int main(int argc,char **argv) {
    if(argc!=2)return 90;
    const char *test=argv[1];
    if(streq(test,"variables")) {
        CHECK(sh_assign("NAME=hello world")==1);
        const char *a[]={"echo","hello world","17","prehello worldpost","$NAME"};
        expect("echo \"$NAME\" $? pre\"${NAME}\"post '$NAME'",17,5,a);
        const char *b[]={"echo","hello","world"};expect("echo $NAME",0,3,b);
        const char *c[]={"echo","",""};expect("echo \"$UNSET\" '' $UNSET",0,3,c);
        CHECK(sh_assign("OP=| > &")==1);
        CHECK(lex("echo \"$OP\"",0)==2);CHECK(!sh_operator(words[1],"|"));
        CHECK(lex("NEW=$NAME",0)==1);CHECK(sh_kind[0]&SH_ASSIGN);CHECK(sh_assign(words[0])==1);CHECK(streq(sh_value("NEW"),"hello world"));
        CHECK(lex("$NEW",0)==2);CHECK(!(sh_kind[0]&SH_ASSIGN));
    } else if(streq(test,"quotes-operators")) {
        const char *a[]={"echo","a b","x|y",">","a&b"};expect("echo a\\ b 'x|y' \\> \"a&b\"",0,5,a);
        int n=lex("echo x|cat>out&",0);CHECK(n==7);CHECK(sh_operator(words[2],"|"));CHECK(sh_operator(words[4],">"));CHECK(sh_operator(words[6],"&"));
        struct stage st[MAX_STAGES];CHECK(parse(words,n-1,st)==2);CHECK(streq(st[1].out_path,"out"));
        n=lex("echo '|' ''",0);CHECK(parse(words,n,st)==1);CHECK(st[0].argc==3);CHECK(!sh_operator(words[1],"|"));
        CHECK(lex("echo \"broken",0)<0);CHECK(lex("echo ${BROKEN",0)<0);CHECK(lex("echo end\\",0)<0);
        n=lex("echo > | cat",0);CHECK(parse(words,n,st)<0);
    } else if(streq(test,"glob")) {
        const char *a[]={"echo","data/a.txt","data/b.txt"};expect("echo data/*.txt",0,3,a);
        const char *b[]={"echo","data/*.txt","data/*.txt","data/no*.txt"};expect("echo 'data/*.txt' data/\\*.txt data/no*.txt",0,4,b);
        const char *c[]={"echo","data/sub/c.txt"};expect("echo data/*/*.txt",0,2,c);
        const char *d[]={"echo","data/.hidden"};expect("echo data/.*",0,2,d);
        int n=lex("echo hi > data/*.txt",0);CHECK(sh_expand(words,n)<0);
        CHECK(sh_assign("PAT=data/*.txt")==1);expect("echo $PAT",0,3,a);
    } else if(streq(test,"glob-io")) {
        read_error=1;int before=open_count,closed=close_count;
        int n=lex("echo data/*",0);CHECK(sh_expand(words,n)<0);CHECK(open_count==before+1);CHECK(close_count==closed+1);
    } else if(streq(test,"limits")) {
        char line[600];memset(line,'a',sizeof(line));line[sizeof(line)-1]=0;CHECK(lex(line,0)<0);
        line[0]=0;for(int i=0;i<95;i++)strcat(line,"x ");CHECK(lex(line,0)<0);
        struct stage st[MAX_STAGES];CHECK(parse(words,lex("echo a b c d e f g h i j k l m n o",0),st)<0);
        for(int i=0;i<SH_VARS;i++){char v[64];snprintf(v,sizeof(v),"N%d=value",i);CHECK(sh_assign(v)==1);}
        CHECK(sh_assign("OVER=bad")<0);CHECK(sh_assign("N0=updated")==1);CHECK(streq(sh_value("N0"),"updated"));
    } else if(streq(test,"editor")) {
        edit("ac\033[Db\n","abc",1);
        edit("axc\033[D\177b\n","abc",1);
        edit("xbc\001\033[3~a\005\n","abc",1);
        edit("draft\025ok\n","ok",1);
        edit("draft\003","",1);
        edit("\004","",0);
    } else if(streq(test,"history")) {
        edit("first\n","first",1);edit("second\n","second",1);
        edit("\033[A\n","second",1);edit("draft\033[A\033[B\n","draft",1);
        for(int i=0;i<25;i++){char s[64];snprintf(s,sizeof(s),"item%d",i);sh_history_add(s);}
        CHECK(sh_history_count==SH_HISTORY);CHECK(streq(sh_history[15],"item24"));
    } else if(streq(test,"completion")) {
        char s[SH_TEXT]="cat data/a";unsigned p=(unsigned)strlen(s);CHECK(sh_complete(s,&p,sizeof(s))==0);CHECK(streq(s,"cat data/a.txt "));
        strcpy(s,"cat data/s");p=(unsigned)strlen(s);CHECK(sh_complete(s,&p,sizeof(s))==0);CHECK(streq(s,"cat data/sub/"));
        strcpy(s,"cat data/");p=(unsigned)strlen(s);CHECK(sh_complete(s,&p,sizeof(s))==0);CHECK(streq(s,"cat data/"));
        strcpy(s,"cat special/a");p=(unsigned)strlen(s);CHECK(sh_complete(s,&p,sizeof(s))==0);CHECK(streq(s,"cat special/a\\ b\\&c "));
        char small[16]="cat data/a";p=(unsigned)strlen(small);CHECK(sh_complete(small,&p,12)<0);CHECK(streq(small,"cat data/a"));
    } else if(streq(test,"fuzz-words")) {
        unsigned rng=12345;
        const char alphabet[]="abc XYZ09_$?{}'\"\\|&<>*#\t/";
        for(int k=0;k<20000;k++) {
            char input[256];unsigned len=(rng>>8)%sizeof(input);
            for(unsigned j=0;j<len;j++) {rng=rng*1664525u+1013904223u;input[j]=alphabet[(rng>>16)%(sizeof(alphabet)-1)];}
            input[len]=0;int n=lex(input,(long)k);
            if(n<0)continue;
            CHECK(n<=SH_WORDS && words[n]==NULL);
            for(int j=0;j<n;j++)CHECK(ustrlen(words[j])<SH_TEXT);
            struct stage st[MAX_STAGES];int ns=n?parse(words,n,st):-1;
            if(ns>0)for(int j=0;j<ns;j++)CHECK(st[j].argc<=MAX_ARGS && st[j].argv[st[j].argc]==NULL);
        }
    } else if(streq(test,"script-main")) {
        char *av[]={"sh","@script",0};
        script_data="NAME='hello world'\nargs \"$NAME\"\nargs $?\nargs '|' ''\nexit\n";
        CHECK(shell_main(2,av)==3);CHECK(run_count==3);
        CHECK(run_argc[0]==2 && streq(run_args[0][1],"hello world"));
        CHECK(run_argc[1]==2 && streq(run_args[1][1],"2"));
        CHECK(run_argc[2]==3 && streq(run_args[2][1],"|") && streq(run_args[2][2],""));
    } else if(streq(test,"script-overflow")) {
        char script[600];memset(script,'a',500);strcpy(script+500,"\nexit\n");
        script_data=script;char *av[]={"sh","@script",0};
        CHECK(shell_main(2,av)==2);CHECK(run_count==0);
    } else return 91;
    CHECK(open_count==close_count);
    printf("%s: %s (%d failure(s))\n",test,failures?"FAIL":"PASS",failures);return failures!=0;
}
