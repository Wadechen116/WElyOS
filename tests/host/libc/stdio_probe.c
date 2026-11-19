/* 原始 libc/ulib/echo 的輸出故障模型：只替換 syscall，不改產品函式。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ulib.h"
#include "contracts.h"
int welyos_stdio_printf(const char *,...);
int welyos_stdio_fprintf(long,const char *,...);
int welyos_stdio_puts(const char *);
int welyos_stdio_putchar(int);
int welyos_stdio_echo_main(int,char **);
int welyos_stdio_cat_main(int,char **);
static const char *fault;
static char emitted[4096];
static size_t emitted_len;
static unsigned calls,read_calls,at;
static size_t read_offset;
long probe_open(const char *path,long flags) { (void)path;(void)flags;return 3; }
long probe_close(long fd) { (void)fd;return 0; }
long probe_read(long fd,void *buf,size_t size) {
    (void)fd;read_calls++;
    if (!strcmp(fault,"read-eio")) return -E_IO;
    if (size>600-read_offset) size=600-read_offset;
    for (size_t i=0;i<size;i++) ((char *)buf)[i]=(char)('a'+(read_offset+i)%26);
    read_offset+=size;return (long)size;
}
long probe_write(long fd,const void *buf,size_t size) {
    (void)fd;
    if (++calls>1024) _Exit(90);
    int active=!at || calls==at;
    if (active && !strcmp(fault,"zero")) return 0;
    if (active && !strcmp(fault,"eio")) return -E_IO;
    if (active && !strcmp(fault,"epipe")) return -E_PIPE;
    if (!strcmp(fault,"eintr") && calls==1) return -E_INTR;
    if (!strcmp(fault,"late-error") && calls==2) return -E_IO;
    size_t accepted=size;
    if (active && !strcmp(fault,"short") && accepted>17) accepted=17;
    if (accepted>sizeof(emitted)-emitted_len) _Exit(91);
    memcpy(emitted+emitted_len,buf,accepted);emitted_len+=accepted;
    return (long)accepted;
}
long probe_sbrk(long delta) { (void)delta;return -E_NOMEM; }
_Noreturn void probe_exit(int status) { _Exit(status); }
int main(int argc,char **argv) {
    if (argc!=3 && argc!=4) return 92;
    at=argc==4 ? (unsigned)strtoul(argv[3],NULL,10):0;
    const char *api=argv[1];fault=argv[2];
    char payload[601],expected[604];
    for (int i=0;i<600;i++) payload[i]=(char)('a'+i%26);
    payload[600]=0;memcpy(expected,payload,sizeof(payload));
    size_t expected_len=600;
    long result=0;int has_status=1;
    if (!strcmp(api,"printf")) result=welyos_stdio_printf("%s",payload);
    else if (!strcmp(api,"fprintf")) result=welyos_stdio_fprintf(2,"%s",payload);
    else if (!strcmp(api,"puts")) {
        expected[600]='\n';expected[601]=0;expected_len++;
        result=welyos_stdio_puts(payload);
    } else if (!strcmp(api,"putchar")) {
        expected[0]='x';expected_len=1;result=welyos_stdio_putchar('x');
    } else if (!strcmp(api,"ulib-output")) {
        result=CALL_PUTS(payload);has_status=PUTS_HAS_STATUS;
    } else if (!strcmp(api,"line-output")) {
        struct line l={0};line_str(&l,payload);expected_len=l.len;
        memcpy(expected,l.buf,expected_len);result=CALL_LINE(&l);has_status=LINE_HAS_STATUS;
    } else if (!strcmp(api,"echo")) {
        char *args[]={"echo","hello",NULL};memcpy(expected,"hello\n",6);expected_len=6;
        result=welyos_stdio_echo_main(2,args);
    } else if (!strncmp(api,"cat-",4)) {
        char *args[]={"cat","input",NULL};
        result=welyos_stdio_cat_main(!strcmp(api,"cat-stdin") ? 1:2,args);
    } else return 93;
    int complete=emitted_len==expected_len && !memcmp(emitted,expected,expected_len);
    int reported_error=has_status && (!strcmp(api,"echo") ? result!=2:(!strncmp(api,"cat-",4) ? result!=0:result<0));
    int normal_ok=strcmp(fault,"normal") || (complete && !reported_error);
    int safe=(complete || reported_error) && normal_ok;
    if (strcmp(api,"echo") && complete && !strcmp(fault,"normal")) {
        if ((!strcmp(api,"printf") || !strcmp(api,"fprintf") || !strcmp(api,"puts")) && result!=(long)expected_len) safe=0;
        if (!strcmp(api,"putchar") && result!='x') safe=0;
    }
    printf("{\"result\":%ld,\"has_status\":%d,\"reported_error\":%d,\"expected_bytes\":%zu,"
           "\"emitted_bytes\":%zu,\"complete\":%d,\"write_calls\":%u}\n",
           result,has_status,reported_error,expected_len,emitted_len,complete,calls);
    return safe ? 0:1;
}
