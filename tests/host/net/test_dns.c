#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../user/dns.h"

#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
static unsigned checks;
static const uint16_t query_id=0x7391;
static const uint32_t address=0xc0000207u;
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0]*256u+p[1]); }
static void wr16(uint8_t *p, unsigned n) { p[0]=(uint8_t)(n>>8); p[1]=(uint8_t)n; }
static size_t name_put(uint8_t *p, const char *s)
{
    size_t n=0;
    while (*s) {
        const char *end=strchr(s,'.'); size_t size=end ? (size_t)(end-s) : strlen(s);
        CHECK(size && size<=63); p[n++]=(uint8_t)size;
        memcpy(p+n,s,size); n+=size; s+=size; if (*s) ++s;
    }
    p[n++]=0; return n;
}
static size_t response(uint8_t *p, const char *name)
{
    memset(p,0,12); wr16(p,query_id); wr16(p+2,0x8180); wr16(p+4,1);
    size_t n=12+name_put(p+12,name); wr16(p+n,1); wr16(p+n+2,1); return n+4;
}
static size_t rr_head(uint8_t *p, size_t n, const char *owner, unsigned type)
{
    if (owner) n+=name_put(p+n,owner);
    else { p[n++]=0xc0; p[n++]=12; }
    wr16(p+n,type); wr16(p+n+2,1); memset(p+n+4,0,4);
    unsigned count=rd16(p+6); wr16(p+6,count+1); return n+8;
}
static size_t a_record(uint8_t *p, size_t n, const char *owner, uint32_t ip)
{
    n=rr_head(p,n,owner,1); wr16(p+n,4); n+=2;
    p[n++]=(uint8_t)(ip>>24); p[n++]=(uint8_t)(ip>>16);
    p[n++]=(uint8_t)(ip>>8); p[n++]=(uint8_t)ip; return n;
}
static size_t cname_record(uint8_t *p, size_t n, const char *owner, const char *target)
{
    n=rr_head(p,n,owner,5); size_t size=name_put(p+n+2,target);
    wr16(p+n,(unsigned)size); return n+2+size;
}
static size_t simple(uint8_t *p)
{
    return a_record(p,response(p,"example.com"),NULL,address);
}
static int parse(const uint8_t *p, size_t n, const char *name, uint32_t *ip)
{
    uint8_t *exact=malloc(n ? n : 1); CHECK(exact);
    if (n) memcpy(exact,p,n);
    uint32_t before=*ip;
    int rc=dns_parse_a(exact,n,query_id,name,ip); free(exact);
    CHECK(rc==0 || *ip==before); return rc;
}
static void bad(const uint8_t *p, size_t n)
{
    uint32_t ip=0xaabbccddu; CHECK(parse(p,n,"example.com",&ip)==-1);
}
static void test_query(void)
{
    static const uint8_t expected[]={0x73,0x91,1,0,0,1,0,0,0,0,0,0,
        7,'e','x','a','m','p','l','e',3,'c','o','m',0,0,1,0,1};
    uint8_t out[300]; size_t n=999;
    CHECK(dns_build_query(out,sizeof(out),query_id,"ExAmPlE.CoM.",&n)==0);
    CHECK(n==sizeof(expected) && !memcmp(out,expected,n));
    for (size_t cap=0;cap<=sizeof(expected)+1;++cap) {
        uint8_t *exact=malloc(cap ? cap : 1); CHECK(exact);
        int rc=dns_build_query(exact,cap,query_id,"example.com",&n);
        CHECK((cap<sizeof(expected) && rc==-1 && n==0) ||
              (cap>=sizeof(expected) && rc==0 && n==sizeof(expected))); free(exact);
    }
    const char *invalid[]={"", ".", "..", "a..b", ".a", "a..", "a b", "a\n", "\x80"};
    for (size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i)
        CHECK(dns_build_query(out,sizeof(out),query_id,invalid[i],&n)==-1 && n==0);
    char long_name[400]; memset(long_name,'a',64); long_name[64]=0;
    CHECK(dns_build_query(out,sizeof(out),query_id,long_name,&n)==-1);
    memset(long_name,'a',sizeof(long_name)); long_name[399]=0;
    CHECK(dns_build_query(out,sizeof(out),query_id,long_name,&n)==-1);
    /* 展開的最大 wire name 是 255 bytes(63.63.63.61)。 */
    memset(long_name,'a',253); long_name[63]=long_name[127]=long_name[191]='.'; long_name[253]=0;
    CHECK(dns_build_query(out,sizeof(out),query_id,long_name,&n)==0 && n==271);
    long_name[253]='.'; long_name[254]=0;
    CHECK(dns_build_query(out,sizeof(out),query_id,long_name,&n)==0 && n==271);
    long_name[253]='a';
    CHECK(dns_build_query(out,sizeof(out),query_id,long_name,&n)==-1);
    CHECK(dns_build_query(NULL,0,1,"a",&n)==-1 && n==0);
    CHECK(dns_build_query(out,sizeof(out),1,NULL,&n)==-1 && n==0);
    CHECK(dns_build_query(out,sizeof(out),1,"a",NULL)==-1);
}
static void test_answers(void)
{
    uint8_t p[4096]; uint32_t ip=0; size_t n=simple(p);
    CHECK(parse(p,n,"EXAMPLE.COM.",&ip)==0 && ip==address);
    /* 不相干的 A 不能被誤當答案,即使它出現在最前面。 */
    n=response(p,"example.com"); n=a_record(p,n,"evil.example",0x01020304u);
    CHECK(parse(p,n,"example.com",&ip)==-2);
    n=a_record(p,n,NULL,address); CHECK(parse(p,n,"example.com",&ip)==0 && ip==address);
    n=response(p,"example.com"); n=a_record(p,n,"target.example",address);
    n=cname_record(p,n,NULL,"TaRgEt.ExAmPlE");
    CHECK(parse(p,n,"example.com",&ip)==0 && ip==address);
    n=response(p,"example.com"); n=cname_record(p,n,NULL,"target.example");
    CHECK(parse(p,n,"example.com",&ip)==-2);
    /* cname 只含已出現的名稱 pointer,驗證 RDLENGTH 不等於展開長度時仍正確。 */
    n=response(p,"example.com"); size_t target=n;
    n=a_record(p,n,"target.example",address); n=rr_head(p,n,NULL,5);
    wr16(p+n,2); wr16(p+n+2,0xc000u|(unsigned)target); n+=4;
    CHECK(parse(p,n,"example.com",&ip)==0 && ip==address);
    n=response(p,"example.com"); wr16(p+2,0x8183);
    CHECK(parse(p,n,"example.com",&ip)==-2);
    n=response(p,"example.com"); CHECK(parse(p,n,"example.com",&ip)==-2);
    n=simple(p); wr16(p+6,0); wr16(p+10,1);
    CHECK(parse(p,n,"example.com",&ip)==-2); /* Additional 不能冒充 Answer */
    n=response(p,"example.com");
    for (unsigned i=0;i<128;++i) n=a_record(p,n,NULL,address);
    CHECK(parse(p,n,"example.com",&ip)==0 && ip==address);
    n=a_record(p,n,NULL,address); CHECK(parse(p,n,"example.com",&ip)==-1);
    n=simple(p); wr16(p+2,0x81b0); CHECK(parse(p,n,"example.com",&ip)==0); /* AD/CD 不當作證據 */
    CHECK(dns_parse_a(NULL,n,query_id,"example.com",&ip)==-1);
    CHECK(dns_parse_a(p,n,query_id,NULL,&ip)==-1);
    CHECK(dns_parse_a(p,n,query_id,"example.com",NULL)==-1);
    CHECK(dns_parse_a(p,n,query_id+1,"example.com",&ip)==-1);
    CHECK(dns_parse_a(p,n,query_id,"other.com",&ip)==-1);
}
static void test_chains(void)
{
    uint8_t p[4096]; uint32_t ip=0;
    for (unsigned hops=1;hops<=9;++hops) {
        size_t n=response(p,"example.com"); char owner[40], target[40];
        for (unsigned h=0;h<hops;++h) {
            snprintf(owner,sizeof(owner),"n%u.example",h);
            snprintf(target,sizeof(target),"n%u.example",h+1);
            n=cname_record(p,n,h ? owner : NULL,target);
        }
        snprintf(target,sizeof(target),"n%u.example",hops); n=a_record(p,n,target,address);
        CHECK(parse(p,n,"example.com",&ip)==(hops<=8 ? 0 : -1));
    }
    size_t n=response(p,"example.com"); n=cname_record(p,n,NULL,"example.com"); bad(p,n);
    n=response(p,"example.com"); n=cname_record(p,n,NULL,"a.example");
    n=cname_record(p,n,"a.example","example.com"); bad(p,n);
    n=response(p,"example.com"); n=cname_record(p,n,NULL,"a.example");
    n=cname_record(p,n,NULL,"b.example"); bad(p,n);
    n=simple(p); n=cname_record(p,n,NULL,"a.example"); bad(p,n);
    /* 名稱壓縮 32 跳可解,33 跳拒絕,即使全部為向後 pointer。 */
    for (unsigned jumps=32;jumps<=33;++jumps) {
        n=response(p,"example.com"); size_t previous=12;
        for (unsigned j=1;j<jumps;++j) {
            n=rr_head(p,n,NULL,99); wr16(p+n,2); n+=2;
            size_t ptr=n; wr16(p+n,0xc000u|(unsigned)previous); n+=2; previous=ptr;
        }
        size_t at=n; wr16(p+n,0xc000u|(unsigned)previous); n+=2;
        wr16(p+n,1); wr16(p+n+2,1); memset(p+n+4,0,4); wr16(p+n+8,4); n+=10;
        p[n++]=192; p[n++]=0; p[n++]=2; p[n++]=7;
        wr16(p+6,rd16(p+6)+1); CHECK(at<n);
        CHECK(parse(p,n,"example.com",&ip)==(jumps==32 ? 0 : -1));
    }
}
static void test_bad_messages(void)
{
    uint8_t p[4096]; size_t n=simple(p);
    for (size_t i=0;i<n;++i) bad(p,i);
    for (unsigned v=0;v<=65535u;++v) {
        n=simple(p); wr16(p+n-6,v); if (v!=4) bad(p,n); /* A RDLENGTH */
        n=simple(p); wr16(p+6,v); if (v!=1) bad(p,n);   /* ANCOUNT */
    }
    for (unsigned bit=0;bit<16;++bit) {
        if (bit==15 || (bit>=11 && bit<=14) || bit==9 || bit==6) {
            n=simple(p); wr16(p+2,rd16(p+2)^(1u<<bit)); bad(p,n);
        }
    }
    n=simple(p); p[0]^=1; bad(p,n);
    n=simple(p); wr16(p+4,2); bad(p,n);
    n=simple(p); p[13]^=1; bad(p,n);
    n=simple(p); wr16(p+25,2); bad(p,n); /* 問題的 type */
    n=simple(p); wr16(p+27,2); bad(p,n); /* 問題的 class */
    n=simple(p); p[n++]=0; bad(p,n);
    n=simple(p); p[12]=0xc0; p[13]=12; bad(p,n); /* 自指 */
    n=simple(p); p[12]=0xc0; p[13]=14; bad(p,n); /* 向前指 */
    n=simple(p); p[12]=0xff; p[13]=255; bad(p,n);
    n=simple(p); p[12]=0x40; bad(p,n); /* 不支援的 label 編碼 */
    n=simple(p); p[12]=63; bad(p,n);
    n=response(p,"example.com"); n=rr_head(p,n,NULL,5); wr16(p+n,2); n+=2;
    wr16(p+n,0xc000u|(unsigned)n); n+=2; bad(p,n); /* CNAME pointer 循環 */
    n=response(p,"example.com"); n=rr_head(p,n,NULL,5); wr16(p+n,2); n+=2;
    wr16(p+n,0xc000u|(unsigned)(n-1)); n+=2; bad(p,n); /* label + pointer 循環 */
    n=response(p,"example.com"); n=rr_head(p,n,NULL,5); wr16(p+n,1); n+=2;
    wr16(p+n,0xc00c); n+=2; bad(p,n); /* RDATA 讀取不得越過其宣告長度 */
    uint32_t ip=0; CHECK(dns_parse_a(p,65536,query_id,"example.com",&ip)==-1);
}
static uint32_t rng=0xa38b4719u;
static uint32_t rand32(void)
{ rng^=rng<<13; rng^=rng>>17; rng^=rng<<5; return rng; }
static void test_random(void)
{
    uint8_t p[1536]; uint32_t ip=0;
    for (unsigned i=0;i<100000;++i) {
        size_t n=rand32()%sizeof(p);
        for (size_t j=0;j<n;++j) p[j]=(uint8_t)rand32();
        (void)parse(p,n,"example.com",&ip);
    }
    for (unsigned i=0;i<100000;++i) {
        size_t n;
        if (i&1u) n=simple(p);
        else {
            n=response(p,"example.com"); n=cname_record(p,n,NULL,"target.example");
            n=a_record(p,n,"target.example",address);
        }
        unsigned changes=1+rand32()%8;
        for (unsigned j=0;j<changes;++j) p[rand32()%n]=(uint8_t)rand32();
        (void)parse(p,n,"example.com",&ip);
    }
    puts("  DNS fixed seed: 100000 raw + 100000 structured PASS");
}
int main(void)
{
    test_query(); test_answers(); test_chains(); test_bad_messages(); test_random();
    printf("dns: PASS (%u checks)\n",checks); return 0;
}
