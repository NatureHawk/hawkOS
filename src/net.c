// src/net.c — Ethernet, ARP, IPv4, ICMP and UDP
//
// One background task drains the NIC's receive ring and pushes each frame up
// through this file; everything above (TCP, DHCP, DNS, HTTP) is driven from
// that single thread of control, so no protocol state needs a lock. Senders
// run on whatever task called them, which is safe because the NIC driver's
// transmit path only touches its own descriptors.
#include <stdint.h>
#include "header/net.h"
#include "header/rtl8139.h"
#include "header/kstring.h"
#include "header/kprintf.h"
#include "header/task.h"
#include "header/tcp.h"

extern volatile unsigned long long ticks;

// ------------------------------------------------------------ wire formats

typedef struct __attribute__((packed)) {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t type;
} eth_hdr_t;

typedef struct __attribute__((packed)) {
    uint16_t htype, ptype;
    uint8_t  hlen, plen;
    uint16_t oper;
    uint8_t  sha[ETH_ALEN];
    uint32_t spa;
    uint8_t  tha[ETH_ALEN];
    uint32_t tpa;
} arp_pkt_t;

typedef struct __attribute__((packed)) {
    uint8_t  ver_ihl;
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t frag;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;
    uint32_t src;
    uint32_t dst;
} ip_hdr_t;

typedef struct __attribute__((packed)) {
    uint8_t  type, code;
    uint16_t checksum;
    uint16_t id, seq;
} icmp_hdr_t;

typedef struct __attribute__((packed)) {
    uint16_t sport, dport, len, checksum;
} udp_hdr_t;

// ------------------------------------------------------------------- state

static int    link_up   = 0;
static ipv4_t local_ip  = 0;
static ipv4_t netmask   = 0;
static ipv4_t gateway   = 0;
static ipv4_t dns_srv   = 0;

static uint16_t ip_ident = 1;

static uint32_t icmp_replies = 0;

static udp_handler_t udp_handler = 0;

static const uint8_t BROADCAST_MAC[ETH_ALEN] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

#define ARP_CACHE_N 16
typedef struct {
    ipv4_t   ip;
    uint8_t  mac[ETH_ALEN];
    int      valid;
    unsigned long long stamp;
} arp_entry_t;
static arp_entry_t arp_cache[ARP_CACHE_N];

// One shared assembly buffer for outgoing frames. Every sender runs to
// completion without yielding, so there is never more than one frame being
// built at a time.
static uint8_t tx_frame[ETH_MTU];
static uint8_t rx_frame[ETH_MTU + 4];

// -------------------------------------------------------------- accessors

int    net_link_up(void){ return link_up; }
int    net_configured(void){ return link_up && local_ip != 0; }
ipv4_t net_local_ip(void){ return local_ip; }
ipv4_t net_netmask(void){ return netmask; }
ipv4_t net_gateway(void){ return gateway; }
ipv4_t net_dns_server(void){ return dns_srv; }
const uint8_t* net_mac(void){ return rtl8139_mac(); }

void net_set_config(ipv4_t ip, ipv4_t mask, ipv4_t gw, ipv4_t dns){
    local_ip = ip; netmask = mask; gateway = gw; dns_srv = dns;
}

char* net_ip_str(ipv4_t ip, char* buf){
    ksnprintf(buf, 16, "%u.%u.%u.%u",
              (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
    return buf;
}

int net_parse_ip(const char* s, ipv4_t* out){
    uint32_t v = 0;
    for (int part = 0; part < 4; part++){
        uint32_t n = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9'){ n = n * 10 + (uint32_t)(*s - '0'); s++; digits++; }
        if (!digits || n > 255) return -1;
        v = (v << 8) | n;
        if (part < 3){ if (*s != '.') return -1; s++; }
    }
    if (*s) return -1;
    *out = v;
    return 0;
}

// The internet checksum: one's-complement sum of 16-bit words, folded and
// inverted. Used verbatim for IP, ICMP, and (over a pseudo-header) UDP/TCP.
uint16_t net_checksum(const void* data, uint32_t len){
    const uint8_t* p = (const uint8_t*)data;
    uint32_t sum = 0;
    while (len > 1){ sum += (uint32_t)((p[0] << 8) | p[1]); p += 2; len -= 2; }
    if (len) sum += (uint32_t)(p[0] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

// --------------------------------------------------------------- ethernet

static int eth_send(const uint8_t dst[ETH_ALEN], uint16_t type,
                    const void* payload, uint16_t len){
    if (!link_up || len > ETH_MTU - (uint16_t)sizeof(eth_hdr_t)) return -1;

    eth_hdr_t* eh = (eth_hdr_t*)tx_frame;
    memcpy(eh->dst, dst, ETH_ALEN);
    memcpy(eh->src, rtl8139_mac(), ETH_ALEN);
    eh->type = htons(type);
    memcpy(tx_frame + sizeof(eth_hdr_t), payload, len);

    return rtl8139_send(tx_frame, (uint16_t)(sizeof(eth_hdr_t) + len));
}

// -------------------------------------------------------------------- ARP

static void arp_store(ipv4_t ip, const uint8_t mac[ETH_ALEN]){
    int slot = -1, oldest = 0;
    for (int i = 0; i < ARP_CACHE_N; i++){
        if (arp_cache[i].valid && arp_cache[i].ip == ip){ slot = i; break; }
        if (!arp_cache[i].valid){ slot = i; break; }
        if (arp_cache[i].stamp < arp_cache[oldest].stamp) oldest = i;
    }
    if (slot < 0) slot = oldest;

    arp_cache[slot].ip    = ip;
    arp_cache[slot].valid = 1;
    arp_cache[slot].stamp = ticks;
    memcpy(arp_cache[slot].mac, mac, ETH_ALEN);
}

int arp_lookup(ipv4_t ip, uint8_t mac_out[ETH_ALEN]){
    for (int i = 0; i < ARP_CACHE_N; i++)
        if (arp_cache[i].valid && arp_cache[i].ip == ip){
            memcpy(mac_out, arp_cache[i].mac, ETH_ALEN);
            return 0;
        }
    return -1;
}

void arp_request(ipv4_t ip){
    arp_pkt_t a;
    memset(&a, 0, sizeof(a));
    a.htype = htons(1);
    a.ptype = htons(ETHERTYPE_IP);
    a.hlen  = ETH_ALEN;
    a.plen  = 4;
    a.oper  = htons(1);                       // request
    memcpy(a.sha, rtl8139_mac(), ETH_ALEN);
    a.spa   = htonl(local_ip);
    a.tpa   = htonl(ip);
    eth_send(BROADCAST_MAC, ETHERTYPE_ARP, &a, sizeof(a));
}

int arp_resolve(ipv4_t ip, uint8_t mac_out[ETH_ALEN], uint32_t timeout_ms){
    if (arp_lookup(ip, mac_out) == 0) return 0;

    unsigned long long deadline = ticks + (timeout_ms + 9) / 10;
    unsigned long long next_try = 0;

    while (ticks < deadline){
        if (ticks >= next_try){ arp_request(ip); next_try = ticks + 50; }
        net_poll();
        if (arp_lookup(ip, mac_out) == 0) return 0;
        task_sleep(10);
    }
    return -1;
}

static void arp_input(const uint8_t* data, uint16_t len){
    if (len < sizeof(arp_pkt_t)) return;
    const arp_pkt_t* a = (const arp_pkt_t*)data;
    if (ntohs(a->ptype) != ETHERTYPE_IP || a->plen != 4) return;

    ipv4_t spa = ntohl(a->spa);
    ipv4_t tpa = ntohl(a->tpa);

    // Learn from every ARP packet we see, request or reply — it is free, and
    // it means a reply to someone else's request still populates the cache.
    if (spa) arp_store(spa, a->sha);

    if (ntohs(a->oper) == 1 && local_ip && tpa == local_ip){
        arp_pkt_t r;
        memset(&r, 0, sizeof(r));
        r.htype = htons(1);
        r.ptype = htons(ETHERTYPE_IP);
        r.hlen  = ETH_ALEN;
        r.plen  = 4;
        r.oper  = htons(2);                   // reply
        memcpy(r.sha, rtl8139_mac(), ETH_ALEN);
        r.spa   = htonl(local_ip);
        memcpy(r.tha, a->sha, ETH_ALEN);
        r.tpa   = a->spa;
        eth_send(a->sha, ETHERTYPE_ARP, &r, sizeof(r));
    }
}

// --------------------------------------------------------------------- IP

int ip_send(uint8_t proto, ipv4_t dst, const void* payload, uint16_t len){
    if (!link_up) return -1;

    uint8_t buf[ETH_MTU - sizeof(eth_hdr_t)];
    if (len + sizeof(ip_hdr_t) > sizeof(buf)) return -1;

    // Anything outside our subnet goes to the gateway. A broadcast address
    // is sent as an Ethernet broadcast without consulting ARP at all, which
    // is what makes DHCP work before we have an address.
    uint8_t dst_mac[ETH_ALEN];
    if (dst == 0xFFFFFFFFu){
        memcpy(dst_mac, BROADCAST_MAC, ETH_ALEN);
    } else {
        ipv4_t next_hop = ((dst ^ local_ip) & netmask) ? gateway : dst;
        if (next_hop == 0) next_hop = dst;
        if (arp_resolve(next_hop, dst_mac, 1500) != 0) return -1;
    }

    ip_hdr_t* ih = (ip_hdr_t*)buf;
    ih->ver_ihl   = 0x45;                     // IPv4, 20-byte header, no options
    ih->tos       = 0;
    ih->total_len = htons((uint16_t)(sizeof(ip_hdr_t) + len));
    ih->id        = htons(ip_ident++);
    ih->frag      = htons(0x4000);            // don't fragment
    ih->ttl       = 64;
    ih->proto     = proto;
    ih->checksum  = 0;
    ih->src       = htonl(local_ip);
    ih->dst       = htonl(dst);
    ih->checksum  = htons(net_checksum(ih, sizeof(ip_hdr_t)));

    memcpy(buf + sizeof(ip_hdr_t), payload, len);
    return eth_send(dst_mac, ETHERTYPE_IP, buf, (uint16_t)(sizeof(ip_hdr_t) + len));
}

// ------------------------------------------------------------------- ICMP

int icmp_ping(ipv4_t dst, uint16_t seq){
    uint8_t pkt[sizeof(icmp_hdr_t) + 32];
    icmp_hdr_t* ic = (icmp_hdr_t*)pkt;
    ic->type = 8;                             // echo request
    ic->code = 0;
    ic->checksum = 0;
    ic->id  = htons(0x4157);          // arbitrary, just has to be echoed back
    ic->seq = htons(seq);
    for (int i = 0; i < 32; i++) pkt[sizeof(icmp_hdr_t) + i] = (uint8_t)('a' + i % 26);
    ic->checksum = htons(net_checksum(pkt, sizeof(pkt)));
    return ip_send(IPPROTO_ICMP, dst, pkt, sizeof(pkt));
}

uint32_t icmp_reply_count(void){ return icmp_replies; }

static void icmp_input(ipv4_t src, const uint8_t* data, uint16_t len){
    if (len < sizeof(icmp_hdr_t)) return;
    const icmp_hdr_t* ic = (const icmp_hdr_t*)data;

    if (ic->type == 0){ icmp_replies++; return; }      // echo reply

    if (ic->type == 8){                                // echo request: answer it
        uint8_t reply[576];
        if (len > sizeof(reply)) return;
        memcpy(reply, data, len);
        icmp_hdr_t* r = (icmp_hdr_t*)reply;
        r->type = 0;
        r->checksum = 0;
        r->checksum = htons(net_checksum(reply, len));
        ip_send(IPPROTO_ICMP, src, reply, len);
    }
}

// -------------------------------------------------------------------- UDP

void udp_set_handler(udp_handler_t fn){ udp_handler = fn; }

int udp_send(ipv4_t dst, uint16_t sport, uint16_t dport, const void* data, uint16_t len){
    uint8_t pkt[1480];
    if (len + sizeof(udp_hdr_t) > sizeof(pkt)) return -1;

    udp_hdr_t* uh = (udp_hdr_t*)pkt;
    uh->sport    = htons(sport);
    uh->dport    = htons(dport);
    uh->len      = htons((uint16_t)(sizeof(udp_hdr_t) + len));
    uh->checksum = 0;                          // optional in IPv4, and omitted
    memcpy(pkt + sizeof(udp_hdr_t), data, len);

    return ip_send(IPPROTO_UDP, dst, pkt, (uint16_t)(sizeof(udp_hdr_t) + len));
}

static void udp_input(ipv4_t src, const uint8_t* data, uint16_t len){
    if (len < sizeof(udp_hdr_t)) return;
    const udp_hdr_t* uh = (const udp_hdr_t*)data;
    uint16_t ulen = ntohs(uh->len);
    if (ulen < sizeof(udp_hdr_t) || ulen > len) return;

    if (udp_handler)
        udp_handler(src, ntohs(uh->sport), ntohs(uh->dport),
                    data + sizeof(udp_hdr_t), (uint16_t)(ulen - sizeof(udp_hdr_t)));
}

// ----------------------------------------------------------------- demux

static void ip_input(const uint8_t* data, uint16_t len){
    if (len < sizeof(ip_hdr_t)) return;
    const ip_hdr_t* ih = (const ip_hdr_t*)data;
    if ((ih->ver_ihl >> 4) != 4) return;

    uint16_t ihl = (uint16_t)((ih->ver_ihl & 0x0F) * 4);
    uint16_t total = ntohs(ih->total_len);
    if (ihl < sizeof(ip_hdr_t) || total < ihl || total > len) return;

    ipv4_t src = ntohl(ih->src);
    ipv4_t dst = ntohl(ih->dst);

    // Accept anything addressed to us or broadcast. During DHCP we have no
    // address yet, so unicast offers to 0.0.0.0 have to be let through too.
    if (local_ip && dst != local_ip && dst != 0xFFFFFFFFu &&
        dst != (local_ip | ~netmask)) return;

    const uint8_t* payload = data + ihl;
    uint16_t plen = (uint16_t)(total - ihl);

    switch (ih->proto){
        case IPPROTO_ICMP: icmp_input(src, payload, plen); break;
        case IPPROTO_UDP:  udp_input(src, payload, plen);  break;
        case IPPROTO_TCP:  tcp_input(src, dst, payload, plen); break;
        default: break;
    }
}

static void eth_input(const uint8_t* frame, uint16_t len){
    if (len < sizeof(eth_hdr_t)) return;
    const eth_hdr_t* eh = (const eth_hdr_t*)frame;
    const uint8_t* payload = frame + sizeof(eth_hdr_t);
    uint16_t plen = (uint16_t)(len - sizeof(eth_hdr_t));

    switch (ntohs(eh->type)){
        case ETHERTYPE_ARP: arp_input(payload, plen); break;
        case ETHERTYPE_IP:  ip_input(payload, plen);  break;
        default: break;
    }
}

void net_poll(void){
    if (!link_up) return;
    // Bounded per call so a flood cannot starve everything else on this task.
    for (int i = 0; i < 32; i++){
        uint16_t n = rtl8139_poll(rx_frame, sizeof(rx_frame));
        if (!n) break;
        eth_input(rx_frame, n);
    }
}

static void net_task(void* arg){
    (void)arg;
    for (;;){
        net_poll();
        tcp_tick();        // retransmissions ride on the same 100 Hz cadence
        task_sleep(10);
    }
}

void net_init(void){
    memset(arp_cache, 0, sizeof(arp_cache));

    if (rtl8139_init() != 0){
        link_up = 0;
        kprintf("[net] no network interface; networking disabled\n");
        return;
    }
    link_up = 1;

    task_create("net", net_task, 0);
    kprintf("[net] stack up, receive task started\n");
}
