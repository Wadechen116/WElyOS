#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#define ROUNDS_MAX 15
struct clockinfo {uint64_t count,hz,kmain,ready;};
static int scenario,calls,numbers,errors;
static uint64_t hz=1000;
static int sys_clock(struct clockinfo *ci) {
    calls++;
    if((scenario==1 && calls==1) || (scenario==2 && calls==2)) return -5;
    ci->count=calls==1?100:scenario==3?99:200;return 0;
}
static int probe_printf(const char *fmt,...) {
    if(strstr(fmt,"ns/op"))numbers++;
    if(strstr(fmt,"FAILED"))errors++;
    va_list ap;va_start(ap,fmt);int rc=vprintf(fmt,ap);va_end(ap);return rc;
}
#define printf probe_printf
#include "functions.c"
#undef printf
static int operation(int n) {(void)n;return 0;}
int main(int argc,char **argv) {
    if(argc!=2)return 90;
    scenario=argv[1][0]-'0';
    run("clock_probe",operation,1,1);
    int safe=scenario==0 ? !failed && numbers==1 : failed && numbers==0;
    printf("scenario=%d failed=%d numeric_rows=%d errors=%d safe=%d\n",scenario,failed,numbers,errors,safe);
    return !safe;
}
