// src/netcfg.c — DHCP client and DNS resolver
//
// Both live here because they share the single UDP demultiplexer that net.c
// exposes: one handler function inspects the destination port and routes to
// whichever of the two is waiting. Keeping that dispatch in one place is
// simpler than giving net.c a port table it would only ever have two
// entries in.
#include <stdint.h>
#include "header/netcfg.h"
#include "header/net.h"
#include "header/kstring.h"
#include "header/kprintf.h"
#include "header/task.h"

extern volatile unsigned long long ticks;

#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67
#define DNS_PORT         53

// ------------------------------------------------------------------- DHCP

typedef struct __attribute__((packed)) {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t cookie;
    uint8_t  options[312];
} dhcp_pkt_t;

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5

static uint32_t dhcp_xid    = 0;
static volatile int dhcp_stage = 0;      // 0 idle, 1 sent discover, 2 sent request, 3 done
static ipv4_t   dhcp_offer_ip = 0;
static ipv4_t   dhcp_server   = 0;
static ipv4_t   dhcp_mask     = 0;
static ipv4_t   dhcp_router   = 0;
static ipv4_t   dhcp_dns      = 0;

// --------------------------------------------------------------------- DNS

#define DNS_CACHE_N 8
typedef struct { char host[64]; ipv4_t ip; int valid; } dns_entry_t;
static dns_entry_t dns_cache[DNS_CACHE_N];

static volatile int      dns_pending  = 0;
static volatile ipv4_t   dns_result   = 0;
static uint16_t          dns_query_id = 0x1234;
static uint16_t          dns_port     = 50000;

// -------------------------------------------------------------- DHCP build

static uint8_t* put_opt(uint8_t* p, uint8_t code, uint8_t len, const void* val){
    *p++ = code;
    *p++ = len;
    memcpy(p, val, len);
    return p + len;
}

static void dhcp_send(uint8_t type, ipv4_t requested, ipv4_t server){
    dhcp_pkt_t d;
    memset(&d, 0, sizeof(d));

    d.op     = 1;                 // BOOTREQUEST
    d.htype  = 1;                 // Ethernet
    d.hlen   = 6;
    d.xid    = htonl(dhcp_xid);
    d.flags  = htons(0x8000);     // ask for a broadcast reply: we have no IP yet
    d.cookie = htonl(0x63825363u);
    memcpy(d.chaddr, net_mac(), 6);

    uint8_t* p = d.options;
    *p++ = 53; *p++ = 1; *p++ = type;

    if (type == DHCP_REQUEST){
        uint32_t v = htonl(requested);
        p = put_opt(p, 50, 4, &v);          // requested address
        v = htonl(server);
        p = put_opt(p, 54, 4, &v);          // server identifier
    }

    static const uint8_t want[] = { 1, 3, 6 };   // subnet mask, router, DNS
    p = put_opt(p, 55, sizeof(want), want);
    *p++ = 255;                                   // end of options

    uint32_t used = (uint32_t)(p - (uint8_t*)&d);
    udp_send(0xFFFFFFFFu, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, &d, (uint16_t)used);
}

static void dhcp_parse_options(const uint8_t* opt, uint16_t len, uint8_t* type_out){
    uint16_t i = 0;
    while (i + 1 < len){
        uint8_t code = opt[i];
        if (code == 255) break;
        if (code == 0){ i++; continue; }          // pad
        uint8_t olen = opt[i + 1];
        const uint8_t* val = opt + i + 2;
        if (i + 2 + olen > len) break;

        switch (code){
            case 53: if (olen >= 1) *type_out = val[0]; break;
            case 1:  if (olen >= 4) dhcp_mask   = ntohl(*(const uint32_t*)val); break;
            case 3:  if (olen >= 4) dhcp_router = ntohl(*(const uint32_t*)val); break;
            case 6:  if (olen >= 4) dhcp_dns    = ntohl(*(const uint32_t*)val); break;
            case 54: if (olen >= 4) dhcp_server = ntohl(*(const uint32_t*)val); break;
            default: break;
        }
        i = (uint16_t)(i + 2 + olen);
    }
}

static void dhcp_input(const uint8_t* data, uint16_t len){
    if (len < 240) return;
    const dhcp_pkt_t* d = (const dhcp_pkt_t*)data;
    if (d->op != 2 || ntohl(d->xid) != dhcp_xid) return;
    if (ntohl(d->cookie) != 0x63825363u) return;

    uint8_t type = 0;
    dhcp_parse_options(d->options, (uint16_t)(len - 240), &type);

    if (type == DHCP_OFFER && dhcp_stage == 1){
        dhcp_offer_ip = ntohl(d->yiaddr);
        dhcp_send(DHCP_REQUEST, dhcp_offer_ip, dhcp_server);
        dhcp_stage = 2;
    } else if (type == DHCP_ACK && dhcp_stage == 2){
        dhcp_offer_ip = ntohl(d->yiaddr);
        dhcp_stage = 3;
    }
}

// --------------------------------------------------------------- DNS wire

// Encodes "www.example.com" as the length-prefixed label sequence DNS uses:
// 3 w w w 7 e x a m p l e 3 c o m 0.
static int dns_encode_name(const char* host, uint8_t* out, int cap){
    int o = 0;
    const char* seg = host;
    while (*seg){
        const char* dot = seg;
        while (*dot && *dot != '.') dot++;
        int seglen = (int)(dot - seg);
        if (seglen <= 0 || seglen > 63 || o + seglen + 1 >= cap) return -1;
        out[o++] = (uint8_t)seglen;
        memcpy(out + o, seg, (uint32_t)seglen);
        o += seglen;
        seg = *dot ? dot + 1 : dot;
    }
    if (o >= cap) return -1;
    out[o++] = 0;
    return o;
}

// Skips a name in the answer section, which may be a pointer (top two bits
// set) into an earlier part of the message rather than inline labels.
static int dns_skip_name(const uint8_t* msg, uint16_t len, uint16_t pos){
    while (pos < len){
        uint8_t l = msg[pos];
        if (l == 0) return pos + 1;
        if ((l & 0xC0) == 0xC0) return pos + 2;
        pos = (uint16_t)(pos + 1 + l);
    }
    return -1;
}

static void dns_input(const uint8_t* msg, uint16_t len){
    if (len < 12 || !dns_pending) return;

    uint16_t id = (uint16_t)((msg[0] << 8) | msg[1]);
    if (id != dns_query_id) return;

    uint16_t qd = (uint16_t)((msg[4] << 8) | msg[5]);
    uint16_t an = (uint16_t)((msg[6] << 8) | msg[7]);
    if (an == 0){ dns_pending = 0; return; }

    uint16_t pos = 12;
    for (uint16_t i = 0; i < qd; i++){
        int n = dns_skip_name(msg, len, pos);
        if (n < 0) return;
        pos = (uint16_t)(n + 4);           // qtype + qclass
    }

    for (uint16_t i = 0; i < an && pos + 10 <= len; i++){
        int n = dns_skip_name(msg, len, pos);
        if (n < 0) return;
        pos = (uint16_t)n;

        uint16_t type   = (uint16_t)((msg[pos] << 8) | msg[pos + 1]);
        uint16_t rdlen  = (uint16_t)((msg[pos + 8] << 8) | msg[pos + 9]);
        pos = (uint16_t)(pos + 10);

        if (type == 1 && rdlen == 4 && pos + 4 <= len){
            dns_result = ((uint32_t)msg[pos] << 24) | ((uint32_t)msg[pos + 1] << 16)
                       | ((uint32_t)msg[pos + 2] << 8) | msg[pos + 3];
            dns_pending = 0;
            return;
        }
        pos = (uint16_t)(pos + rdlen);
    }
    dns_pending = 0;
}

// ------------------------------------------------------------ UDP dispatch

static void udp_dispatch(ipv4_t src, uint16_t sport, uint16_t dport,
                         const uint8_t* data, uint16_t len){
    (void)src;
    if (dport == DHCP_CLIENT_PORT && sport == DHCP_SERVER_PORT) dhcp_input(data, len);
    else if (sport == DNS_PORT)                                 dns_input(data, len);
}

// --------------------------------------------------------------- public

int dhcp_run(uint32_t timeout_ms){
    if (!net_link_up()) return -1;

    dhcp_xid    = (uint32_t)(ticks * 2654435761u) | 1u;
    dhcp_stage  = 1;
    dhcp_mask   = 0; dhcp_router = 0; dhcp_dns = 0; dhcp_server = 0;

    net_set_config(0, 0, 0, 0);          // send from 0.0.0.0 until we are told otherwise
    dhcp_send(DHCP_DISCOVER, 0, 0);

    unsigned long long deadline = ticks + (timeout_ms + 9) / 10;
    unsigned long long resend   = ticks + 100;

    while (ticks < deadline && dhcp_stage != 3){
        net_poll();
        if (ticks >= resend){
            // Retry from the top: an OFFER we missed is not worth tracking,
            // and a fresh DISCOVER re-triggers the whole exchange.
            if (dhcp_stage == 1)      dhcp_send(DHCP_DISCOVER, 0, 0);
            else if (dhcp_stage == 2) dhcp_send(DHCP_REQUEST, dhcp_offer_ip, dhcp_server);
            resend = ticks + 100;
        }
        task_sleep(10);
    }

    if (dhcp_stage != 3){
        kprintf("[dhcp] no reply; leaving the interface unconfigured\n");
        return -1;
    }

    if (!dhcp_mask) dhcp_mask = IP4(255,255,255,0);
    net_set_config(dhcp_offer_ip, dhcp_mask, dhcp_router, dhcp_dns);

    char a[16], b[16], c[16], d[16];
    kprintf("[dhcp] ip=%s mask=%s gw=%s dns=%s\n",
            net_ip_str(dhcp_offer_ip, a), net_ip_str(dhcp_mask, b),
            net_ip_str(dhcp_router, c),   net_ip_str(dhcp_dns, d));
    return 0;
}

int netcfg_init(uint32_t timeout_ms){
    memset(dns_cache, 0, sizeof(dns_cache));
    udp_set_handler(udp_dispatch);
    return dhcp_run(timeout_ms);
}

void dns_cache_clear(void){ memset(dns_cache, 0, sizeof(dns_cache)); }

static void dns_cache_put(const char* host, ipv4_t ip){
    for (int i = 0; i < DNS_CACHE_N; i++){
        if (!dns_cache[i].valid){
            strncpy(dns_cache[i].host, host, sizeof(dns_cache[i].host) - 1);
            dns_cache[i].host[sizeof(dns_cache[i].host) - 1] = 0;
            dns_cache[i].ip    = ip;
            dns_cache[i].valid = 1;
            return;
        }
    }
    // Full: overwrite the first slot rather than growing the table.
    strncpy(dns_cache[0].host, host, sizeof(dns_cache[0].host) - 1);
    dns_cache[0].host[sizeof(dns_cache[0].host) - 1] = 0;
    dns_cache[0].ip = ip;
}

int dns_resolve(const char* host, ipv4_t* out, uint32_t timeout_ms){
    if (!host || !*host) return -1;

    if (net_parse_ip(host, out) == 0) return 0;      // already a dotted quad

    for (int i = 0; i < DNS_CACHE_N; i++)
        if (dns_cache[i].valid && kstricmp(dns_cache[i].host, host) == 0){
            *out = dns_cache[i].ip;
            return 0;
        }

    ipv4_t server = net_dns_server();
    if (!server) return -1;

    uint8_t q[300];
    dns_query_id = (uint16_t)(dns_query_id * 31u + 17u);

    q[0] = (uint8_t)(dns_query_id >> 8); q[1] = (uint8_t)dns_query_id;
    q[2] = 0x01; q[3] = 0x00;                        // standard query, recursion desired
    q[4] = 0; q[5] = 1;                              // one question
    q[6] = 0; q[7] = 0;
    q[8] = 0; q[9] = 0;
    q[10] = 0; q[11] = 0;

    int n = dns_encode_name(host, q + 12, (int)sizeof(q) - 12 - 4);
    if (n < 0) return -1;
    int qlen = 12 + n;
    q[qlen++] = 0; q[qlen++] = 1;                    // QTYPE  = A
    q[qlen++] = 0; q[qlen++] = 1;                    // QCLASS = IN

    dns_result  = 0;
    dns_pending = 1;
    if (++dns_port > 60000) dns_port = 50000;

    udp_send(server, dns_port, DNS_PORT, q, (uint16_t)qlen);

    unsigned long long deadline = ticks + (timeout_ms + 9) / 10;
    unsigned long long resend   = ticks + 100;

    while (ticks < deadline && dns_pending){
        net_poll();
        if (ticks >= resend){
            udp_send(server, dns_port, DNS_PORT, q, (uint16_t)qlen);
            resend = ticks + 100;
        }
        task_sleep(10);
    }

    if (!dns_result){ dns_pending = 0; return -1; }

    *out = dns_result;
    dns_cache_put(host, dns_result);
    return 0;
}
