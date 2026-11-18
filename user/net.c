/*
 * net.c - 顯示網路的設定與統計
 * 用法:net
 */
#include "libc.h"

static void show_ip(const char *label, unsigned ip)
{
    printf("%-9s%u.%u.%u.%u\n", label, ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

int main(void)
{
    struct netinfo ni;

    if (sys_netinfo(&ni) != 0 || !ni.up) {
        printf("net: no network\n");
        return 1;
    }
    printf("%-9s%02x:%02x:%02x:%02x:%02x:%02x\n", "mac", ni.mac[0], ni.mac[1], ni.mac[2], ni.mac[3],
           ni.mac[4], ni.mac[5]);
    show_ip("address", ni.ip);
    show_ip("netmask", ni.netmask);
    show_ip("gateway", ni.gateway);
    printf("frames   received %u, sent %u, send errors %u\n", ni.rx_frames, ni.tx_frames, ni.tx_errors);
    printf("dropped  %u (bad length %u, bad checksum %u, not for us %u, no listener %u)\n",
           ni.rx_dropped, ni.rx_bad_length, ni.rx_bad_checksum, ni.rx_filtered, ni.rx_unbound);
    printf("arp      %u cached, %u timed out, %u packets refused while waiting\n",
           ni.arp_cache_entries, ni.arp_timeouts, ni.tx_queue_full);
    printf("answered %u pings; delivered %u UDP packets (%u dropped: reader too slow)\n",
           ni.icmp_echo_replied, ni.udp_delivered, ni.sock_dropped);
    return 0;
}
