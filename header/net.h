#pragma once
#include <stdint.h>
#include "header/rtl8139.h"

// IPv4 addresses are kept in host byte order everywhere above the wire
// format, and converted only when a header is built or parsed. Mixing the
// two is the classic source of networking bugs, so the rule here is: if it
// is in a struct that goes on the wire it is big-endian, and everything else
// is native.
typedef uint32_t ipv4_t;

#define IP4(a,b,c,d) (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(uint32_t)(d))

#define ETHERTYPE_IP   0x0800
#define ETHERTYPE_ARP  0x0806

#define IPPROTO_ICMP   1
#define IPPROTO_TCP    6
#define IPPROTO_UDP    17

static inline uint16_t htons(uint16_t v){ return (uint16_t)((v << 8) | (v >> 8)); }
static inline uint16_t ntohs(uint16_t v){ return htons(v); }
static inline uint32_t htonl(uint32_t v){
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8)
         | ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}
static inline uint32_t ntohl(uint32_t v){ return htonl(v); }

// ---------------------------------------------------------------- lifecycle

// Brings up the NIC and starts the background network task that drains the
// receive ring. Safe to call when no NIC is present: everything below then
// reports "down" rather than failing.
void net_init(void);
int  net_link_up(void);          // a NIC exists and is initialised
int  net_configured(void);       // we also have an IP address

ipv4_t net_local_ip(void);
ipv4_t net_netmask(void);
ipv4_t net_gateway(void);
ipv4_t net_dns_server(void);
void   net_set_config(ipv4_t ip, ipv4_t mask, ipv4_t gw, ipv4_t dns);

const uint8_t* net_mac(void);

// Formats a.b.c.d into buf (at least 16 bytes) and returns it.
char* net_ip_str(ipv4_t ip, char* buf);
// Parses "a.b.c.d". Returns 0 on success.
int   net_parse_ip(const char* s, ipv4_t* out);

uint16_t net_checksum(const void* data, uint32_t len);

// Drains one batch of received frames. Called by the network task; also
// called directly by blocking helpers so they make progress while waiting.
void net_poll(void);

// --------------------------------------------------------------------- ARP

// Looks up `ip` in the ARP cache. On a miss it sends a request and returns
// -1 immediately — callers poll rather than block inside the resolver.
int  arp_lookup(ipv4_t ip, uint8_t mac_out[ETH_ALEN]);
void arp_request(ipv4_t ip);
// Resolves with a bounded wait, pumping the receive path while it waits.
int  arp_resolve(ipv4_t ip, uint8_t mac_out[ETH_ALEN], uint32_t timeout_ms);

// ---------------------------------------------------------------------- IP

// Sends one IPv4 packet. Off-subnet destinations are sent to the gateway's
// MAC, which is the whole of the routing logic this stack needs.
int ip_send(uint8_t proto, ipv4_t dst, const void* payload, uint16_t len);

// -------------------------------------------------------------------- ICMP

int      icmp_ping(ipv4_t dst, uint16_t seq);
uint32_t icmp_reply_count(void);

// --------------------------------------------------------------------- UDP

typedef void (*udp_handler_t)(ipv4_t src, uint16_t sport, uint16_t dport,
                              const uint8_t* data, uint16_t len);
void udp_set_handler(udp_handler_t fn);
int  udp_send(ipv4_t dst, uint16_t sport, uint16_t dport, const void* data, uint16_t len);

// TCP's receive path is a plain callback from the IP demultiplexer, declared
// here so net.c can dispatch into tcp.c without a circular include.
void tcp_input(ipv4_t src, ipv4_t dst, const uint8_t* seg, uint16_t len);
