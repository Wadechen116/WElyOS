/*
 * netif.c - 網路的整合層(說明見 netif.h)
 *
 * 資料的流向:
 *
 *   接收   網路卡 --recv--> [net 執行緒] --鎖--> net_rx() --回呼--> socket 的佇列 / ping 的等待者
 *   傳送   系統呼叫 --鎖--> net_udp_send() / net_ping_send() --nif.send--> 網路卡
 *
 * 一把鎖(net_lk)包住所有進入協定堆疊的呼叫。堆疊在鎖裡面呼叫的回呼只做兩件事:
 * 把資料複製到固定大小的佇列、設定旗標;不呼叫任何 net_* 函式,也不等待。
 * 等待的一方(系統呼叫)在鎖外以 1ms 的間隔檢查旗標 — 沒有額外的同步機制,
 * 延遲的上限就是一個 tick,而且每一步都看得出最多會等多久。
 *
 * 記憶體全部是靜態的:8 個 socket,每個可以暫存 4 個收到的封包。
 * 封包來得比程式讀得快時,多的直接丟掉並計數(UDP 本來就不保證送達)。
 */
#include "cred.h"
#include "fs.h"
#include "kprintf.h"
#include "kstring.h"
#include "net.h"
#include "netdev.h"
#include "netif.h"
#include "sched.h"
#include "sync.h"
#include "timer.h"

#define MAX_SOCKS   8u
#define SOCK_QUEUE  4u                       /* 每個 socket 最多暫存幾個收到的封包 */
#define MAX_PINGS   4u                       /* 同時最多幾個行程在等 ping 的回答 */
#define EPHEMERAL   49152u                   /* 系統自動挑選的埠從這裡開始 */

struct dgram {
    uint32_t ip;
    uint16_t port, len;
    uint8_t data[NET_UDP_MAX];
};

struct usock {
    struct inode in;                         /* 必須是第一個成員:VFS 以它代表這個 socket */
    int used;
    uint16_t port;
    unsigned head, count;                    /* 佇列:環狀,受 net_lk 保護 */
    struct dgram q[SOCK_QUEUE];
};

struct pinger {
    int used, done;
    uint16_t id, seq;
};

static struct mutex net_lk = MUTEX_INIT("net", MX_NET);
static struct netdev *nic;
static struct netif nif;
static int up;
static struct usock socks[MAX_SOCKS];
static struct pinger pingers[MAX_PINGS];
static uint16_t next_port = EPHEMERAL, next_ping_id = 1;
static uint32_t sock_dropped;
static uint8_t rx_frame[NET_FRAME_MAX + 2];
static const struct inode_ops sock_ops;

static uint32_t now_ms(void)
{
    return (uint32_t)timer_ticks();          /* 1 tick = 1ms;只取低 32 位元(堆疊以模 2^32 的差計時) */
}

/* 堆疊要送出一個訊框(在 net_lk 裡面被呼叫):直接交給驅動程式,它在返回前複製走 */
static int nif_send(struct netif *n, const void *frame, size_t len)
{
    (void)n;
    return nic->send(nic, frame, len);
}

/* ---- 回呼(在 net_lk 裡面,由 net_rx 呼叫):只複製與設旗標 ---- */

static void on_udp(void *ctx, uint32_t src_ip, uint16_t src_port, const void *data, size_t len)
{
    struct usock *s = ctx;

    if (!s->used || len > NET_UDP_MAX || s->count == SOCK_QUEUE) {
        sock_dropped++;                      /* 程式來不及讀:丟掉這一個 */
        return;
    }
    struct dgram *d = &s->q[(s->head + s->count++) % SOCK_QUEUE];
    d->ip = src_ip;
    d->port = src_port;
    d->len = (uint16_t)len;
    memcpy(d->data, data, len);
}

static void on_ping(void *ctx, uint32_t src_ip, uint16_t id, uint16_t seq, const void *data, size_t len)
{
    (void)ctx; (void)src_ip; (void)data; (void)len;
    for (struct pinger *p = pingers; p < pingers + MAX_PINGS; p++)
        if (p->used && p->id == id && p->seq == seq)
            p->done = 1;
}

/* ---- 接收與計時的執行緒 ---- */

static void net_thread(void *arg)
{
    uint32_t last_tick = now_ms();

    (void)arg;
    for (;;) {
        int n;
        /* 把驅動程式裡已經收到的都拿出來(一輪最多 32 個:別的執行緒也要用鎖) */
        for (int i = 0; i < 32 && (n = nic->recv(nic, rx_frame, NET_FRAME_MAX)) > 0; i++) {
            mutex_lock(&net_lk);
            net_rx(&nif, rx_frame, (size_t)n);
            mutex_unlock(&net_lk);
        }
        uint32_t t = now_ms();
        if (t - last_tick >= 100) {          /* 每 100ms 推動一次堆疊的計時(ARP 的重試與過期) */
            last_tick = t;
            mutex_lock(&net_lk);
            net_tick(t);
            mutex_unlock(&net_lk);
        }
        thread_sleep(1);
    }
}

void netif_start(struct netdev *dev)
{
    if (!dev || up)
        return;
    nic = dev;
    memcpy(nif.mac, dev->mac, 6);
    /* 位址是固定的:QEMU 使用者模式網路給客體的設定(沒有做自動取得位址的協定) */
    nif.ip = NET_IP(10, 0, 2, 15);
    nif.netmask = NET_IP(255, 255, 255, 0);
    nif.gateway = NET_IP(10, 0, 2, 2);
    nif.send = nif_send;
    for (unsigned i = 0; i < MAX_SOCKS; i++)
        socks[i].used = 0;

    mutex_lock(&net_lk);
    net_init(&nif);
    net_tick(now_ms());                      /* 先讓堆疊知道現在的時間(net.h 規則 8) */
    net_ping_handler(on_ping, NULL);
    mutex_unlock(&net_lk);
    if (thread_create("net", net_thread, NULL) < 0) {
        kprintf("net: cannot start the receive thread\n");
        return;
    }
    up = 1;
    kprintf("net: up, 10.0.2.15/24, gateway 10.0.2.2\n");
}

/* 等到 *flag 不為 0、逾時、或呼叫者被訊號打斷。回傳 1 = 等到了 */
static int wait_flag(volatile int *flag, unsigned timeout_ms)
{
    uint64_t dl = timer_deadline_ms(timeout_ms);  /* 至少等 timeout_ms(見 timer.h) */

    while (!*flag) {
        if (timer_expired(dl) || thread_interrupted())
            return 0;
        thread_sleep(1);
    }
    return 1;
}

long netif_ping(uint32_t ip, unsigned timeout_ms)
{
    static const char payload[] = "WElyOS ping payload 0123456789abcdef";
    struct pinger *p = NULL;
    long rc;

    if (!up)
        return -E_NETDOWN;
    mutex_lock(&net_lk);
    for (struct pinger *q = pingers; q < pingers + MAX_PINGS && !p; q++)
        if (!q->used)
            p = q;
    if (!p) {
        mutex_unlock(&net_lk);
        return -E_BUSY;
    }
    p->used = 1;
    p->done = 0;
    p->id = next_ping_id++;
    p->seq = 1;
    uint64_t t0 = timer_ticks();
    rc = net_ping_send(ip, p->id, p->seq, payload, sizeof(payload));
    mutex_unlock(&net_lk);

    if (rc >= 0)
        rc = wait_flag(&p->done, timeout_ms) ? (long)(timer_ticks() - t0) : -E_TIMEDOUT;
    mutex_lock(&net_lk);
    p->used = 0;
    mutex_unlock(&net_lk);
    return rc;
}

/* ---- UDP socket ---- */

static struct usock *sock_of(struct file *f)
{
    struct inode *in = vfs_file_inode(f);

    return in && in->ops == &sock_ops ? (struct usock *)in : NULL;
}

int netif_udp_open(uint16_t port, struct file **out)
{
    const struct cred *c = cred_current();
    struct usock *s = NULL;
    int err = 0;

    if (!up)
        return -E_NETDOWN;
    if (port && port < 1024 && c && !(c->caps & CAP_ADMIN))
        return -E_ACCES;                     /* 知名的埠只給有特權的行程 */
    mutex_lock(&net_lk);
    for (struct usock *q = socks; q < socks + MAX_SOCKS && !s; q++)
        if (!q->used)
            s = q;
    if (!s) {
        err = -E_NOMEM;
    } else if (port) {
        err = net_udp_bind(port, on_udp, s);
    } else {
        /* 自動挑選:從上次的下一個開始試,最多繞一圈 */
        err = -E_BUSY;
        for (unsigned tries = 0; tries < 65536u - EPHEMERAL && err == -E_BUSY; tries++) {
            port = next_port;
            next_port = next_port == 65535u ? (uint16_t)EPHEMERAL : (uint16_t)(next_port + 1);
            err = net_udp_bind(port, on_udp, s);
        }
    }
    if (err == 0) {
        inode_init(&s->in, &sock_ops, I_SOCK);
        s->used = 1;
        s->port = port;
        s->head = s->count = 0;
    }
    mutex_unlock(&net_lk);
    if (err)
        return err;
    *out = vfs_file_new(&s->in, O_RDWR);
    if (!*out) {
        mutex_lock(&net_lk);
        net_udp_unbind(port);
        s->used = 0;
        mutex_unlock(&net_lk);
        return -E_NOMEM;
    }
    return 0;
}

/* 沒有任何描述符指向這個 socket 了(關閉,或行程結束時由 kernel 代為關閉):釋放它 */
static void sock_release(struct inode *in)
{
    struct usock *s = (struct usock *)in;

    mutex_lock(&net_lk);
    if (s->used) {
        net_udp_unbind(s->port);
        s->used = 0;
    }
    mutex_unlock(&net_lk);
}

static const struct inode_ops sock_ops = { .release = sock_release };

long netif_udp_send(struct file *f, uint32_t ip, uint16_t port, const void *data, size_t len)
{
    struct usock *s = sock_of(f);

    if (!s)
        return -E_BADF;
    if (len > NET_UDP_MAX)
        return -E_INVAL;
    mutex_lock(&net_lk);
    int err = net_udp_send(s->port, ip, port, data, len);
    mutex_unlock(&net_lk);
    return err < 0 ? err : (long)len;
}

long netif_udp_recv(struct file *f, uint32_t *ip, uint16_t *port, void *buf, size_t cap,
                    unsigned timeout_ms)
{
    struct usock *s = sock_of(f);
    uint64_t dl = timer_deadline_ms(timeout_ms);  /* 0 = 只看一次;否則至少等 timeout_ms */
    long rc = -E_TIMEDOUT;

    if (!s)
        return -E_BADF;
    for (;;) {
        mutex_lock(&net_lk);
        if (s->count > 0) {
            struct dgram *d = &s->q[s->head];
            size_t n = d->len < cap ? d->len : cap;
            memcpy(buf, d->data, n);
            *ip = d->ip;
            *port = d->port;
            s->head = (s->head + 1) % SOCK_QUEUE;
            s->count--;
            rc = (long)n;
        }
        mutex_unlock(&net_lk);
        if (rc >= 0 || timer_expired(dl))
            break;
        if (thread_interrupted()) {
            rc = -E_INTR;
            break;
        }
        thread_sleep(1);
    }
    return rc;
}

void netif_info(struct netinfo *out)
{
    struct net_stats st;

    memset(out, 0, sizeof(*out));
    if (!up)
        return;
    mutex_lock(&net_lk);
    net_get_stats(&st);
    out->sock_dropped = sock_dropped;
    mutex_unlock(&net_lk);
    out->up = 1;
    memcpy(out->mac, nif.mac, 6);
    out->ip = nif.ip;
    out->netmask = nif.netmask;
    out->gateway = nif.gateway;
    out->rx_frames = st.rx_frames;
    out->tx_frames = st.tx_frames;
    out->rx_dropped = st.rx_dropped;
    out->rx_bad_length = st.rx_bad_length;
    out->rx_bad_checksum = st.rx_bad_checksum;
    out->rx_filtered = st.rx_filtered;
    out->rx_unbound = st.rx_unbound;
    out->tx_errors = st.tx_errors;
    out->tx_queue_full = st.tx_queue_full;
    out->arp_timeouts = st.arp_timeouts;
    out->arp_cache_entries = st.arp_cache_entries;
    out->icmp_echo_replied = st.icmp_echo_replied;
    out->udp_delivered = st.udp_delivered;
}
