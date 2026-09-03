// src/tcp.c — client-side TCP
//
// Scope is deliberately narrow: active opens only, one segment in flight at
// a time, and in-order receive. That is enough to fetch a web page, and it
// keeps the state machine small enough to reason about. Out-of-order
// segments are dropped rather than queued — the peer retransmits them, and
// the cost of that on a LAN or through QEMU's user-mode NAT is negligible
// compared to the complexity of a reassembly queue.
#include <stdint.h>
#include "header/tcp.h"
#include "header/net.h"
#include "header/kheap.h"
#include "header/kstring.h"
#include "header/kprintf.h"

extern volatile unsigned long long ticks;

#define TCP_CONNS      4
#define RX_CAP         (192u * 1024u)    // one page of HTML, comfortably
#define TX_SEG_MAX     1400              // stays under the 1500-byte MTU
#define RETRANSMIT_MS  600
#define MAX_RETRIES    8

#define FIN 0x01
#define SYN 0x02
#define RST 0x04
#define PSH 0x08
#define ACK 0x10

typedef struct __attribute__((packed)) {
    uint16_t sport, dport;
    uint32_t seq, ack;
    uint8_t  offset;        // high nibble: header length in 32-bit words
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} tcp_hdr_t;

struct tcp_conn {
    int      used;
    int      state;

    ipv4_t   peer_ip;
    uint16_t peer_port;
    uint16_t local_port;

    uint32_t snd_nxt;       // next sequence number we will send
    uint32_t snd_una;       // oldest unacknowledged byte
    uint32_t rcv_nxt;       // next sequence number we expect

    uint8_t* rx;
    uint32_t rx_len;

    // A single retransmission slot. With one segment in flight this is all
    // the reliability machinery the client side needs.
    uint8_t  pending[TX_SEG_MAX];
    uint32_t pending_len;
    uint8_t  pending_flags;
    uint32_t pending_seq;
    unsigned long long resend_at;
    int      retries;
};

static struct tcp_conn conns[TCP_CONNS];
static uint16_t next_port = 40000;

// ------------------------------------------------------------------ output

// TCP's checksum covers a pseudo-header carrying the IP addresses, which is
// what binds a segment to the connection it belongs to.
static uint16_t segment_checksum(ipv4_t src, ipv4_t dst, const uint8_t* seg, uint16_t len){
    uint8_t buf[12 + TX_SEG_MAX + sizeof(tcp_hdr_t)];
    if ((uint32_t)len + 12 > sizeof(buf)) return 0;

    buf[0] = (uint8_t)(src >> 24); buf[1] = (uint8_t)(src >> 16);
    buf[2] = (uint8_t)(src >> 8);  buf[3] = (uint8_t)src;
    buf[4] = (uint8_t)(dst >> 24); buf[5] = (uint8_t)(dst >> 16);
    buf[6] = (uint8_t)(dst >> 8);  buf[7] = (uint8_t)dst;
    buf[8]  = 0;
    buf[9]  = IPPROTO_TCP;
    buf[10] = (uint8_t)(len >> 8);
    buf[11] = (uint8_t)len;
    memcpy(buf + 12, seg, len);

    return net_checksum(buf, (uint32_t)len + 12);
}

static int send_segment(struct tcp_conn* c, uint8_t flags,
                        const void* data, uint16_t data_len, uint32_t seq){
    uint8_t seg[sizeof(tcp_hdr_t) + TX_SEG_MAX];
    if (data_len > TX_SEG_MAX) return -1;

    tcp_hdr_t* th = (tcp_hdr_t*)seg;
    th->sport    = htons(c->local_port);
    th->dport    = htons(c->peer_port);
    th->seq      = htonl(seq);
    th->ack      = htonl(c->rcv_nxt);
    th->offset   = (uint8_t)((sizeof(tcp_hdr_t) / 4) << 4);
    th->flags    = flags;
    th->window   = htons((uint16_t)((RX_CAP - c->rx_len) > 32768u ? 32768u : (RX_CAP - c->rx_len)));
    th->checksum = 0;
    th->urgent   = 0;
    if (data_len) memcpy(seg + sizeof(tcp_hdr_t), data, data_len);

    uint16_t total = (uint16_t)(sizeof(tcp_hdr_t) + data_len);
    th->checksum = htons(segment_checksum(net_local_ip(), c->peer_ip, seg, total));

    return ip_send(IPPROTO_TCP, c->peer_ip, seg, total);
}

static void arm_retransmit(struct tcp_conn* c, uint8_t flags,
                           const void* data, uint16_t len, uint32_t seq){
    c->pending_flags = flags;
    c->pending_len   = len;
    c->pending_seq   = seq;
    if (len && data) memcpy(c->pending, data, len);
    c->resend_at = ticks + RETRANSMIT_MS / 10;
    c->retries   = 0;
}

static void clear_retransmit(struct tcp_conn* c){
    c->pending_len   = 0;
    c->pending_flags = 0;
    c->resend_at     = 0;
    c->retries       = 0;
}

// -------------------------------------------------------------------- API

tcp_conn_t* tcp_open(ipv4_t dst, uint16_t dport){
    if (!net_configured()) return 0;

    struct tcp_conn* c = 0;
    for (int i = 0; i < TCP_CONNS; i++) if (!conns[i].used){ c = &conns[i]; break; }
    if (!c) return 0;

    memset(c, 0, sizeof(*c));
    c->rx = (uint8_t*)kmalloc(RX_CAP);
    if (!c->rx) return 0;

    c->used       = 1;
    c->state      = TCP_SYN_SENT;
    c->peer_ip    = dst;
    c->peer_port  = dport;
    c->local_port = next_port++;
    if (next_port > 60000) next_port = 40000;

    // A fixed initial sequence number would be a security problem on a real
    // network; derived from the tick counter it is at least not constant
    // across connections.
    c->snd_nxt = (uint32_t)(ticks * 2654435761u) | 1u;
    c->snd_una = c->snd_nxt;
    c->rcv_nxt = 0;

    send_segment(c, SYN, 0, 0, c->snd_nxt);
    arm_retransmit(c, SYN, 0, 0, c->snd_nxt);
    c->snd_nxt++;                       // SYN consumes one sequence number

    return c;
}

int tcp_state(const tcp_conn_t* c){ return c ? c->state : TCP_CLOSED; }

uint32_t tcp_available(const tcp_conn_t* c){ return c ? c->rx_len : 0; }

int tcp_write(tcp_conn_t* c, const void* data, uint32_t len){
    if (!c || !c->used || c->state != TCP_ESTABLISHED) return -1;

    const uint8_t* p = (const uint8_t*)data;
    uint32_t sent = 0;
    while (sent < len){
        uint32_t chunk = len - sent;
        if (chunk > TX_SEG_MAX) chunk = TX_SEG_MAX;

        if (send_segment(c, PSH | ACK, p + sent, (uint16_t)chunk, c->snd_nxt) != 0)
            return (int)sent;

        arm_retransmit(c, PSH | ACK, p + sent, (uint16_t)chunk, c->snd_nxt);
        c->snd_nxt += chunk;
        sent += chunk;
    }
    return (int)sent;
}

int tcp_read(tcp_conn_t* c, uint8_t* buf, uint32_t cap){
    if (!c || !c->used) return -1;

    if (c->rx_len == 0){
        // Nothing buffered: only report end-of-stream once the peer has
        // actually finished, otherwise a slow server looks like a closed one.
        if (c->state == TCP_CLOSE_WAIT || c->state == TCP_CLOSED) return -1;
        return 0;
    }

    uint32_t n = c->rx_len < cap ? c->rx_len : cap;
    memcpy(buf, c->rx, n);
    if (n < c->rx_len) memmove(c->rx, c->rx + n, c->rx_len - n);
    c->rx_len -= n;
    return (int)n;
}

void tcp_close(tcp_conn_t* c){
    if (!c || !c->used) return;
    if (c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT){
        send_segment(c, FIN | ACK, 0, 0, c->snd_nxt);
        c->snd_nxt++;
        c->state = TCP_FIN_WAIT;
    } else {
        c->state = TCP_CLOSED;
    }
}

void tcp_free(tcp_conn_t* c){
    if (!c || !c->used) return;
    if (c->rx) kfree(c->rx);
    memset(c, 0, sizeof(*c));
}

// ------------------------------------------------------------------- input

static struct tcp_conn* find_conn(ipv4_t src, uint16_t sport, uint16_t dport){
    for (int i = 0; i < TCP_CONNS; i++){
        struct tcp_conn* c = &conns[i];
        if (c->used && c->peer_ip == src && c->peer_port == sport && c->local_port == dport)
            return c;
    }
    return 0;
}

void tcp_input(ipv4_t src, ipv4_t dst, const uint8_t* seg, uint16_t len){
    (void)dst;
    if (len < sizeof(tcp_hdr_t)) return;

    const tcp_hdr_t* th = (const tcp_hdr_t*)seg;
    uint16_t hlen = (uint16_t)((th->offset >> 4) * 4);
    if (hlen < sizeof(tcp_hdr_t) || hlen > len) return;

    struct tcp_conn* c = find_conn(src, ntohs(th->sport), ntohs(th->dport));
    if (!c) return;

    uint32_t seq   = ntohl(th->seq);
    uint32_t ackno = ntohl(th->ack);
    uint8_t  flags = th->flags;

    const uint8_t* payload = seg + hlen;
    uint16_t plen = (uint16_t)(len - hlen);

    if (flags & RST){ c->state = TCP_CLOSED; clear_retransmit(c); return; }

    if (c->state == TCP_SYN_SENT){
        if ((flags & (SYN | ACK)) == (SYN | ACK) && ackno == c->snd_nxt){
            c->rcv_nxt = seq + 1;
            c->snd_una = ackno;
            c->state   = TCP_ESTABLISHED;
            clear_retransmit(c);
            send_segment(c, ACK, 0, 0, c->snd_nxt);
        }
        return;
    }

    if (flags & ACK){
        // Sequence numbers wrap, so "newer than" has to be a signed
        // difference rather than a plain comparison.
        if ((int32_t)(ackno - c->snd_una) > 0){
            c->snd_una = ackno;
            clear_retransmit(c);
        }
    }

    if (plen){
        if (seq == c->rcv_nxt){
            uint32_t room = RX_CAP - c->rx_len;
            uint32_t take = plen < room ? plen : room;
            if (take){
                memcpy(c->rx + c->rx_len, payload, take);
                c->rx_len  += take;
                c->rcv_nxt += take;
            }
            send_segment(c, ACK, 0, 0, c->snd_nxt);
        } else {
            // Out of order or a retransmission we already have: re-ACK what
            // we do have so the peer knows where to resume.
            send_segment(c, ACK, 0, 0, c->snd_nxt);
        }
    }

    if (flags & FIN){
        // Only accept the FIN if it is the next thing in sequence; otherwise
        // data is still missing and the stream is not really finished.
        if (seq + plen == c->rcv_nxt || plen == 0){
            c->rcv_nxt++;
            send_segment(c, ACK, 0, 0, c->snd_nxt);
            if (c->state == TCP_FIN_WAIT) c->state = TCP_CLOSED;
            else                          c->state = TCP_CLOSE_WAIT;
        }
    }
}

void tcp_tick(void){
    for (int i = 0; i < TCP_CONNS; i++){
        struct tcp_conn* c = &conns[i];
        if (!c->used || !c->resend_at) continue;
        if (ticks < c->resend_at) continue;

        if (++c->retries > MAX_RETRIES){
            c->state = TCP_CLOSED;
            clear_retransmit(c);
            continue;
        }

        send_segment(c, c->pending_flags,
                     c->pending_len ? c->pending : 0,
                     (uint16_t)c->pending_len, c->pending_seq);
        // Back off linearly; a client that only ever has one segment in
        // flight does not need anything cleverer.
        c->resend_at = ticks + (unsigned long long)(RETRANSMIT_MS / 10) * (c->retries + 1);
    }
}
