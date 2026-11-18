#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "net.h"
#include "fs.h"

#define CHECK(x) do { ++checks; if (!(x)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static unsigned checks, sent, fail_send, got_udp, got_ping;
static size_t last_len, got_len;
static uint8_t last[NET_FRAME_MAX], got_data[NET_UDP_MAX];
static uint32_t got_src;
static uint16_t got_port, got_id, got_seq;
static const uint8_t peer_mac[6] = {2,0,0,0,0,2};
static struct netif nif = {{2,0,0,0,0,15}, NET_IP(10,0,2,15),
                           NET_IP(255,255,255,0), NET_IP(10,0,2,2), NULL, NULL};
static uint32_t peer = NET_IP(10,0,2,2);
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0]*256u+p[1]); }
static uint32_t get32(const uint8_t *p)
{ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static void set16(uint8_t *p, unsigned n) { p[0]=(uint8_t)(n>>8); p[1]=(uint8_t)n; }
static void set32(uint8_t *p, uint32_t n)
{ p[0]=(uint8_t)(n>>24); p[1]=(uint8_t)(n>>16); p[2]=(uint8_t)(n>>8); p[3]=(uint8_t)n; }
/* 測試用獨立實作,避免拿產品函式驗證產品自己的檢查碼。 */
static uint16_t checksum(const uint8_t *p, size_t n, uint32_t sum)
{
    for (size_t i=0; i<n; ++i) sum += (uint32_t)p[i] << ((i&1u) ? 0 : 8);
    while (sum>>16) sum=(sum&65535u)+(sum>>16);
    return (uint16_t)~sum;
}
static uint32_t pseudo(uint32_t src, uint32_t dst, size_t n)
{ return (src>>16)+(src&65535u)+(dst>>16)+(dst&65535u)+17u+(uint32_t)n; }
static int fake_send(struct netif *n, const void *p, size_t len)
{
    CHECK(n==&nif); CHECK(len>=14 && len<=NET_FRAME_MAX);
    if (fail_send) return -E_IO;
    ++sent; memcpy(last,p,len); last_len=len; return 0;
}
static void udp_cb(void *ctx, uint32_t src, uint16_t port, const void *p, size_t n)
{
    CHECK(ctx==&got_udp); CHECK(n<=sizeof(got_data)); ++got_udp;
    got_src=src; got_port=port; got_len=n; memcpy(got_data,p,n);
}
static void ping_cb(void *ctx, uint32_t src, uint16_t id, uint16_t seq,
                    const void *p, size_t n)
{
    CHECK(ctx==&got_ping); CHECK(n<=sizeof(got_data)); ++got_ping;
    got_src=src; got_id=id; got_seq=seq; got_len=n; memcpy(got_data,p,n);
}
static void reset(uint32_t now)
{
    nif.send=fake_send; sent=fail_send=got_udp=got_ping=0;
    last_len=got_len=0; net_init(&nif); net_tick(now);
}
static struct net_stats snapshot(void)
{
    struct net_stats s; net_get_stats(&s);
    CHECK(s.rx_dropped==s.rx_bad_length+s.rx_bad_checksum+s.rx_filtered+s.rx_unbound);
    return s;
}
static void receive(const uint8_t *p, size_t n)
{
    /* 每個截斷點都使用精確配置大小,讓 sanitizer 也能看見 len 之外的讀取。 */
    uint8_t *copy=malloc(n ? n : 1); CHECK(copy!=NULL);
    if (n) memcpy(copy,p,n);
    net_rx(&nif,copy,n); free(copy); (void)snapshot();
}
static void ethernet(uint8_t *f, unsigned type)
{
    memcpy(f,nif.mac,6); memcpy(f+6,peer_mac,6); set16(f+12,type);
}
static size_t arp(uint8_t *f, unsigned op, uint32_t src)
{
    memset(f,0,42); ethernet(f,0x0806);
    set16(f+14,1); set16(f+16,0x0800); f[18]=6; f[19]=4;
    set16(f+20,op); memcpy(f+22,peer_mac,6); set32(f+28,src);
    memcpy(f+32,nif.mac,6); set32(f+38,nif.ip); return 42;
}
static void fix_ip(uint8_t *f)
{ set16(f+24,0); set16(f+24,checksum(f+14,20,0)); }
static void ip(uint8_t *f, unsigned proto, size_t n)
{
    memset(f,0,34+n); ethernet(f,0x0800); f[14]=0x45;
    set16(f+16,(unsigned)(20+n)); f[22]=64; f[23]=(uint8_t)proto;
    set32(f+26,peer); set32(f+30,nif.ip); fix_ip(f);
}
static size_t ping_frame(uint8_t *f, unsigned type, size_t n)
{
    ip(f,1,8+n); uint8_t *p=f+34; p[0]=(uint8_t)type;
    set16(p+4,0x1234); set16(p+6,0x5678);
    for (size_t i=0; i<n; ++i) p[8+i]=(uint8_t)(i*37u);
    set16(p+2,checksum(p,8+n,0)); return 42+n;
}
static size_t udp_frame(uint8_t *f, unsigned port, size_t n, int with_sum)
{
    ip(f,17,8+n); uint8_t *p=f+34;
    set16(p,53); set16(p+2,port); set16(p+4,(unsigned)(8+n));
    for (size_t i=0; i<n; ++i) p[8+i]=(uint8_t)(i*19u);
    if (with_sum) {
        uint16_t sum=checksum(p,8+n,pseudo(peer,nif.ip,8+n));
        set16(p+6,sum ? sum : 65535u);
    }
    return 42+n;
}
static void resolve(void)
{
    uint8_t f[42]; receive(f,arp(f,1,peer));
    CHECK(snapshot().arp_cache_entries==1); sent=0;
}
static void test_arp(void)
{
    uint8_t f[64]; reset(100);
    CHECK(net_ping_send(peer,1,2,"abc",3)==0);
    CHECK(sent==1 && last_len==42 && get16(last+20)==1);
    CHECK(get32(last+38)==peer);
    CHECK(net_udp_send(9,NET_IP(8,8,8,8),53,NULL,0)==-E_BUSY);
    CHECK(snapshot().tx_queue_full==1);
    receive(f,arp(f,2,peer));
    CHECK(sent==2 && last_len==45 && get16(last+12)==0x0800);
    CHECK(!memcmp(last,peer_mac,6) && !memcmp(last+42,"abc",3));
    CHECK(checksum(last+14,20,0)==0 && checksum(last+34,11,0)==0);
    CHECK(snapshot().arp_cache_entries==1);
    /* 未經請求的 reply 與不同 MAC 的 request 都不能覆寫有效紀錄。 */
    arp(f,2,peer); f[11]=f[27]=3; receive(f,42);
    arp(f,1,peer); f[11]=f[27]=3; receive(f,42);
    CHECK(snapshot().rx_filtered==2);
    CHECK(net_ping_send(peer,1,2,NULL,0)==0 && !memcmp(last,peer_mac,6));
    net_tick(60099); CHECK(snapshot().arp_cache_entries==1);
    net_tick(60100); CHECK(snapshot().arp_cache_entries==0);
    CHECK(net_ping_send(peer,1,2,NULL,0)==0 && get16(last+12)==0x0806);
    arp(f,2,peer); f[11]=f[27]=3; receive(f,42);
    CHECK(snapshot().arp_cache_entries==1 && last[5]==3);
    reset(0); receive(f,arp(f,1,peer));
    CHECK(sent==1 && get16(last+20)==2 && get32(last+38)==peer);
    CHECK(snapshot().arp_replies_sent==1);
    reset(0); arp(f,1,0); receive(f,42);
    CHECK(sent==1 && snapshot().arp_cache_entries==0);
    reset(0); arp(f,1,peer); f[22]^=4; receive(f,42);
    CHECK(sent==0 && snapshot().rx_filtered==1);
    reset(0); receive(f,arp(f,2,peer)); CHECK(snapshot().arp_cache_entries==0);
    reset(0); arp(f,1,peer); set32(f+38,nif.ip+1); receive(f,42);
    CHECK(sent==0 && snapshot().arp_cache_entries==0);
}
static void test_timers_capacity(void)
{
    reset(0xfffffff0u);
    CHECK(net_ping_send(peer,1,1,NULL,0)==0);
    net_tick(0xfffffff0u+999u); CHECK(sent==1);
    for (uint32_t i=1;i<=3;++i) {
        net_tick(0xfffffff0u+i*1000u); CHECK(sent==i+1);
    }
    net_tick(0xfffffff0u+3999u); CHECK(snapshot().arp_timeouts==0);
    net_tick(0xfffffff0u+4000u); CHECK(snapshot().arp_timeouts==1 && sent==4);
    CHECK(net_ping_send(peer,1,1,NULL,0)==0);
    reset(123); CHECK(net_ping_send(peer,1,1,NULL,0)==0);
    net_tick(10123); CHECK(sent==1 && snapshot().arp_timeouts==1);
    reset(0);
    for (unsigned i=1;i<=8;++i)
        CHECK(net_ping_send(NET_IP(10,0,2,100)+i,1,1,NULL,0)==0);
    CHECK(net_ping_send(NET_IP(10,0,2,200),1,1,NULL,0)==-E_NOMEM);
    CHECK(snapshot().tx_queue_full==1); net_tick(4000);
    CHECK(snapshot().arp_timeouts==8);
    CHECK(net_ping_send(peer,1,1,NULL,0)==0);
    reset(0); uint8_t f[42];
    for (unsigned i=1;i<=8;++i) receive(f,arp(f,1,NET_IP(10,0,2,100)+i));
    CHECK(snapshot().arp_cache_entries==8);
    receive(f,arp(f,1,NET_IP(10,0,2,200))); CHECK(snapshot().arp_cache_entries==8);
    reset(0xfffffff0u); resolve();
    net_tick(0xfffffff0u+59999u); CHECK(snapshot().arp_cache_entries==1);
    net_tick(0xfffffff0u+60000u); CHECK(snapshot().arp_cache_entries==0);
}
static void test_arp_matching(void)
{
    uint8_t f[64], data[3]={11,22,33}; reset(0);
    CHECK(net_udp_send(123,peer,53,data,3)==0);
    memset(data,99,sizeof(data));
    arp(f,2,peer); f[32]^=4;
    receive(f,42); CHECK(sent==1 && snapshot().arp_cache_entries==0);
    arp(f,2,peer); set32(f+38,nif.ip+1); receive(f,42);
    CHECK(sent==1 && snapshot().arp_cache_entries==0);
    arp(f,2,peer+1); receive(f,42); CHECK(sent==1);
    arp(f,2,peer); f[22]^=4; receive(f,42); CHECK(sent==1);
    arp(f,2,peer); memset(f,255,6); receive(f,42); CHECK(sent==1);
    receive(f,arp(f,2,peer));
    CHECK(sent==2 && last[42]==11 && last[43]==22 && last[44]==33);
    receive(f,arp(f,2,peer)); CHECK(sent==2);
    /* 針對本機的 request 亦可完成等待中的解析,但其他主機的 request 不可。 */
    reset(0); CHECK(net_ping_send(peer,1,1,NULL,0)==0);
    receive(f,arp(f,1,peer)); CHECK(sent==3 && snapshot().arp_cache_entries==1);
    CHECK(snapshot().arp_replies_sent==1);
    reset(0); fail_send=1; receive(f,arp(f,1,peer));
    CHECK(snapshot().tx_errors==1 && snapshot().arp_replies_sent==0);
}
static void test_icmp(void)
{
    uint8_t f[NET_FRAME_MAX]; reset(0);
    size_t n=ping_frame(f,8,7); receive(f,n);
    CHECK(sent==1 && last[34]==0 && last_len==n);
    CHECK(get32(last+26)==nif.ip && get32(last+30)==peer);
    CHECK(checksum(last+14,20,0)==0 && checksum(last+34,n-34,0)==0);
    CHECK(snapshot().icmp_echo_replied==1);
    net_ping_handler(ping_cb,&got_ping); receive(f,ping_frame(f,0,9));
    CHECK(got_ping==1 && got_src==peer && got_id==0x1234 && got_seq==0x5678 && got_len==9);
    ping_frame(f,8,7); f[42]^=1; receive(f,49); CHECK(snapshot().rx_bad_checksum==1);
    ping_frame(f,8,7); set32(f+30,0xffffffffu); fix_ip(f); receive(f,49);
    ping_frame(f,8,7); set32(f+30,nif.ip|~nif.netmask); fix_ip(f); receive(f,49);
    ping_frame(f,8,7); set32(f+30,NET_IP(224,0,0,1)); fix_ip(f); receive(f,49);
    ping_frame(f,8,7); memset(f,255,6); receive(f,49);
    CHECK(sent==1 && snapshot().rx_filtered==4);
    reset(0); resolve();
    uint8_t data[NET_PING_MAX]; memset(data,0x5a,sizeof(data));
    CHECK(net_ping_send(peer,0x1234,0x5678,data,sizeof(data))==0);
    CHECK(last_len==NET_FRAME_MAX && checksum(last+34,1480,0)==0);
    receive(f,ping_frame(f,8,NET_PING_MAX)); CHECK(last_len==NET_FRAME_MAX);
}
static void test_udp(void)
{
    uint8_t f[NET_FRAME_MAX]; reset(0);
    CHECK(net_udp_bind(123,udp_cb,&got_udp)==0);
    CHECK(net_udp_bind(123,udp_cb,&got_udp)==-E_BUSY);
    CHECK(net_udp_bind(0,udp_cb,NULL)==-E_INVAL && net_udp_bind(1,NULL,NULL)==-E_INVAL);
    receive(f,udp_frame(f,123,7,1));
    CHECK(got_udp==1 && got_src==peer && got_port==53 && got_len==7 && got_data[6]==114);
    receive(f,udp_frame(f,123,0,0)); CHECK(got_udp==2);
    receive(f,udp_frame(f,456,2,1)); CHECK(snapshot().rx_unbound==1);
    udp_frame(f,123,7,1); f[42]^=1; receive(f,49); CHECK(snapshot().rx_bad_checksum==1);
    net_udp_unbind(123); receive(f,udp_frame(f,123,0,0)); CHECK(got_udp==2);
    for (unsigned i=1;i<=8;++i) CHECK(net_udp_bind((uint16_t)i,udp_cb,&got_udp)==0);
    CHECK(net_udp_bind(9,udp_cb,&got_udp)==-E_NOMEM);
    net_udp_unbind(4); CHECK(net_udp_bind(9,udp_cb,&got_udp)==0);
    reset(0); resolve();
    CHECK(net_udp_send(123,peer,53,"abc",3)==0);
    CHECK(last_len==45 && get16(last+34)==123 && get16(last+36)==53);
    CHECK(checksum(last+34,11,pseudo(nif.ip,peer,11))==0 && get16(last+40)!=0);
    /* 檢查「計算結果為零」在線上必須編碼成全一。 */
    uint8_t word[2]; uint8_t u[10]={0};
    set16(u,123); set16(u+2,53); set16(u+4,10);
    set16(word,checksum(u,10,pseudo(nif.ip,peer,10)));
    CHECK(net_udp_send(123,peer,53,word,2)==0 && get16(last+40)==65535u);
    CHECK(checksum(last+34,10,pseudo(nif.ip,peer,10))==0);
    uint8_t data[NET_UDP_MAX]; memset(data,0x37,sizeof(data));
    CHECK(net_udp_send(123,peer,53,data,sizeof(data))==0 && last_len==NET_FRAME_MAX);
    CHECK(checksum(last+34,1480,pseudo(nif.ip,peer,1480))==0);
    CHECK(net_udp_bind(123,udp_cb,&got_udp)==0);
    receive(f,udp_frame(f,123,NET_UDP_MAX,1)); CHECK(got_len==NET_UDP_MAX);
}
static void test_errors(void)
{
    reset(0); fail_send=1;
    CHECK(net_ping_send(peer,1,1,NULL,0)==-E_IO && snapshot().tx_errors==1);
    fail_send=0; CHECK(net_ping_send(peer,1,1,NULL,0)==0);
    fail_send=1; net_tick(1000); CHECK(snapshot().tx_errors==2);
    uint8_t f[64]; receive(f,arp(f,2,peer)); CHECK(snapshot().tx_errors==3);
    CHECK(net_ping_send(peer,1,1,NULL,0)==-E_IO && snapshot().tx_errors==4);
    CHECK(net_ping_send(peer,1,1,NULL,1)==-E_INVAL);
    CHECK(net_ping_send(peer,1,1,"x",NET_PING_MAX+1)==-E_INVAL);
    CHECK(net_udp_send(1,peer,2,"x",NET_UDP_MAX+1)==-E_INVAL);
    CHECK(net_udp_send(1,peer,0,NULL,0)==-E_INVAL);
    CHECK(net_ping_send(0xffffffffu,1,1,NULL,0)==-E_INVAL);
    CHECK(net_ping_send(NET_IP(224,0,0,1),1,1,NULL,0)==-E_INVAL);
    CHECK(net_ping_send(nif.ip,1,1,NULL,0)==-E_INVAL);
    uint32_t gw=nif.gateway; nif.gateway=0;
    CHECK(net_ping_send(NET_IP(8,8,8,8),1,1,NULL,0)==-E_INVAL); nif.gateway=gw;
    net_get_stats(NULL); net_init(NULL);
    CHECK(net_ping_send(peer,1,1,NULL,0)==-E_INVAL); net_tick(5000);
    reset(0); net_rx(&nif,NULL,0); CHECK(snapshot().rx_bad_length==1);
    arp(f,1,peer); net_rx(NULL,f,42); CHECK(snapshot().rx_filtered==1);
    uint32_t saved=nif.ip; nif.ip=0; receive(f,arp(f,1,peer));
    CHECK(sent==0); nif.ip=saved;
    reset(0); CHECK(net_ping_send(NET_IP(8,8,8,8),1,1,NULL,0)==0);
    CHECK(get32(last+38)==nif.gateway);
    receive(f,arp(f,2,peer)); CHECK(get32(last+30)==NET_IP(8,8,8,8));
}
static void expect_drop(uint8_t *f, size_t n)
{
    uint32_t old=snapshot().rx_dropped; receive(f,n);
    CHECK(snapshot().rx_dropped==old+1);
}
static void test_malformed(void)
{
    uint8_t f[NET_FRAME_MAX]; reset(0);
    for (unsigned kind=0;kind<3;++kind) {
        size_t n=kind==0 ? arp(f,1,peer) : kind==1 ? ping_frame(f,8,13) : udp_frame(f,123,13,1);
        for (size_t i=0;i<n;++i) expect_drop(f,i);
    }
    /* 對所有 16 位元長度值做完整掃描,不只選幾個邊界值。 */
    for (unsigned v=0;v<=65535u;++v) {
        ping_frame(f,8,13); set16(f+16,v); fix_ip(f);
        if (v!=41u) expect_drop(f,55);
        udp_frame(f,123,13,0); set16(f+38,v);
        if (v!=21u) expect_drop(f,55);
    }
    for (unsigned v=0;v<256;++v) {
        arp(f,1,peer); f[18]=(uint8_t)v; if (v!=6) expect_drop(f,42);
        arp(f,1,peer); f[19]=(uint8_t)v; if (v!=4) expect_drop(f,42);
        ping_frame(f,8,13); f[14]=(uint8_t)v; fix_ip(f);
        if (v!=0x45) expect_drop(f,55);
    }
    ping_frame(f,8,13); f[24]^=1; expect_drop(f,55);
    ping_frame(f,8,13); f[42]^=1; expect_drop(f,55);
    ping_frame(f,8,13); f[22]=0; fix_ip(f); expect_drop(f,55);
    for (unsigned bit=0;bit<16;++bit) {
        if (bit==14) continue; /* DF 不代表分段。 */
        ping_frame(f,8,13); set16(f+20,1u<<bit); fix_ip(f); expect_drop(f,55);
    }
    ping_frame(f,8,13); set32(f+26,0); fix_ip(f); expect_drop(f,55);
    /* 乙太網路最小訊框補零是合法的,但超過 MTU 的輸入仍要拒絕。 */
    size_t n=ping_frame(f,8,0); memset(f+n,0,60-n); receive(f,60);
    uint8_t huge[NET_FRAME_MAX+1]={0}; expect_drop(huge,sizeof(huge));
    puts("  truncations + exhaustive length fields: PASS");
}
static uint32_t random_state=0x8172ab39u;
static uint32_t random32(void)
{
    random_state^=random_state<<13; random_state^=random_state>>17;
    random_state^=random_state<<5; return random_state;
}
static void test_random(void)
{
    uint8_t f[NET_FRAME_MAX+16]; reset(0);
    for (unsigned k=0;k<100000;++k) {
        size_t n=random32()%sizeof(f);
        for (size_t i=0;i<n;++i) f[i]=(uint8_t)random32();
        receive(f,n);
    }
    /* 有效外層加上受損內層,讓隨機測試深入協定解析路徑。 */
    for (unsigned k=0;k<100000;++k) {
        size_t n;
        if (k%3==0) n=arp(f,1,peer);
        else if (k%3==1) n=ping_frame(f,8,random32()%100);
        else n=udp_frame(f,123,random32()%100,1);
        unsigned mutations=1+random32()%8;
        for (unsigned j=0;j<mutations;++j) f[random32()%n]=(uint8_t)random32();
        if (k%3 && (k&1)) fix_ip(f);
        receive(f,n);
        if (k%100==0) net_tick(k);
    }
    puts("  fixed seed random frames: 100000 raw + 100000 structured PASS");
}
int main(void)
{
    test_arp(); test_timers_capacity(); test_arp_matching(); test_icmp(); test_udp(); test_errors();
    test_malformed(); test_random();
    printf("net: PASS (%u checks)\n",checks); return 0;
}
