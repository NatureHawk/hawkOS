#pragma once
#include <stdint.h>
#include "header/net.h"

enum {
    TCP_CLOSED = 0,
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_CLOSE_WAIT,     // peer sent FIN; we may still read what is buffered
    TCP_FIN_WAIT,
    TCP_TIME_WAIT
};

typedef struct tcp_conn tcp_conn_t;

// Opens an active connection. Returns immediately in TCP_SYN_SENT — poll
// tcp_state() rather than blocking, so the UI stays responsive while the
// handshake completes.
tcp_conn_t* tcp_open(ipv4_t dst, uint16_t dport);

int      tcp_state(const tcp_conn_t* c);
uint32_t tcp_available(const tcp_conn_t* c);

// Queues data for transmission. This stack sends a segment immediately and
// keeps one copy for retransmission, so a write larger than the peer's MSS
// is split across several calls internally.
int tcp_write(tcp_conn_t* c, const void* data, uint32_t len);

// Copies up to cap bytes out of the receive buffer. Returns the count, 0 if
// nothing is buffered yet, or -1 when the peer has closed and the buffer is
// drained — which is how an HTTP/1.0 response signals end of body.
int tcp_read(tcp_conn_t* c, uint8_t* buf, uint32_t cap);

void tcp_close(tcp_conn_t* c);
void tcp_free(tcp_conn_t* c);

// Retransmission and timeout processing; driven from the network task.
void tcp_tick(void);
