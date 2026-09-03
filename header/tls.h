#pragma once
#include <stdint.h>
#include "header/tcp.h"

// TLS client, layered directly on a connected tcp_conn_t. The interface is
// deliberately the same shape as the TCP one so http.c can hold a pointer to
// either and treat them identically.
typedef struct tls_conn tls_conn_t;

// 0 when this build has no TLS engine linked in, in which case https URLs
// are rejected with a clear message instead of failing obscurely.
int tls_available(void);

// Runs the handshake to completion. `host` is sent as SNI and checked
// against the certificate. Returns 0 on failure.
tls_conn_t* tls_client_open(tcp_conn_t* tcp, const char* host);

int  tls_write(tls_conn_t* t, const void* data, uint32_t len);

// Same contract as tcp_read: >0 bytes, 0 for "nothing yet", -1 at end of
// stream.
int  tls_read(tls_conn_t* t, uint8_t* buf, uint32_t cap);

void tls_close(tls_conn_t* t);

const char* tls_last_error(void);
