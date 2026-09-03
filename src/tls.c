// src/tls.c — TLS client, BearSSL on top of the kernel's TCP
//
// BearSSL was chosen because it is written for exactly this situation: no
// libc beyond a few string routines, no dynamic allocation of its own, and
// no assumptions about an operating system underneath. The whole library
// compiles against the kernel's freestanding toolchain unchanged; everything
// specific to hawkOS is in this one file.
//
// SECURITY NOTE, stated plainly: certificates are parsed but NOT verified
// against a trust anchor store. hawkOS ships no root CA bundle and has no
// reliable wall-clock source to check validity dates against, so a custom
// X.509 engine below accepts any well-formed chain and hands its public key
// to the handshake. That gives real encryption on the wire and defeats
// passive eavesdropping, but NOT an active man-in-the-middle. Do not treat
// this browser as safe for anything that matters.
#include <stdint.h>
#include "header/tls.h"
#include "header/tcp.h"
#include "header/kheap.h"
#include "header/kstring.h"
#include "header/rtc.h"
#include "header/task.h"
#include "header/kprintf.h"

#include "bearssl.h"

extern volatile unsigned long long ticks;

#define IO_TIMEOUT_MS 15000

static char last_error[96] = "";

const char* tls_last_error(void){ return last_error; }
int tls_available(void){ return 1; }

// ------------------------------------------------- permissive X.509 engine

typedef struct {
    const br_x509_class*    vtable;
    br_x509_decoder_context dc;
    int                     cert_index;
    int                     decoder_ok;
} nocheck_x509_t;

static void xwc_start_chain(const br_x509_class** ctx, const char* server_name){
    nocheck_x509_t* x = (nocheck_x509_t*)ctx;
    (void)server_name;
    x->cert_index = 0;
    x->decoder_ok = 0;
}

static void xwc_start_cert(const br_x509_class** ctx, uint32_t length){
    nocheck_x509_t* x = (nocheck_x509_t*)ctx;
    (void)length;
    // Only the end-entity certificate matters here: it carries the public
    // key the handshake needs. The rest of the chain would only be useful
    // for a validation step we are not doing.
    if (x->cert_index == 0) br_x509_decoder_init(&x->dc, 0, 0);
}

static void xwc_append(const br_x509_class** ctx, const unsigned char* buf, size_t len){
    nocheck_x509_t* x = (nocheck_x509_t*)ctx;
    if (x->cert_index == 0) br_x509_decoder_push(&x->dc, buf, len);
}

static void xwc_end_cert(const br_x509_class** ctx){
    nocheck_x509_t* x = (nocheck_x509_t*)ctx;
    if (x->cert_index == 0) x->decoder_ok = (br_x509_decoder_last_error(&x->dc) == 0);
    x->cert_index++;
}

static unsigned xwc_end_chain(const br_x509_class** ctx){
    nocheck_x509_t* x = (nocheck_x509_t*)ctx;
    if (x->cert_index == 0) return BR_ERR_X509_EMPTY_CHAIN;
    if (!x->decoder_ok)     return BR_ERR_X509_BAD_DN;      // malformed, not merely untrusted
    return 0;
}

static const br_x509_pkey* xwc_get_pkey(const br_x509_class* const* ctx, unsigned* usages){
    nocheck_x509_t* x = (nocheck_x509_t*)ctx;
    if (usages) *usages = BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN;
    return br_x509_decoder_get_pkey(&x->dc);
}

static const br_x509_class NOCHECK_VTABLE = {
    sizeof(nocheck_x509_t),
    xwc_start_chain,
    xwc_start_cert,
    xwc_append,
    xwc_end_cert,
    xwc_end_chain,
    xwc_get_pkey
};

// ----------------------------------------------------------- connection

struct tls_conn {
    br_ssl_client_context cc;

    // br_ssl_client_init_full() initialises a full br_x509_minimal_context
    // through the pointer it is given, so it must be given real storage for
    // one. Handing it the (much smaller) nocheck struct instead overruns
    // several hundred bytes of whatever follows. This field exists purely to
    // absorb that write; the engine is swapped out immediately afterwards.
    br_x509_minimal_context xc_minimal;
    nocheck_x509_t        xc;
    br_sslio_context      io;
    tcp_conn_t*           tcp;
    unsigned char*        iobuf;
    int                   eof;
};

static uint64_t read_tsc(void){
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// BearSSL will not start a handshake without entropy, and hawkOS has no
// hardware RNG. The seed below mixes the cycle counter (which varies with
// every interrupt and cache miss), the tick count and the CMOS clock. That
// is enough unpredictability for a hobby OS on an emulator; it would not be
// enough on a machine holding anything valuable.
static void seed_entropy(br_ssl_client_context* cc){
    uint8_t seed[32];
    rtc_time_t t;
    rtc_read(&t);

    uint64_t a = read_tsc();
    for (int i = 0; i < 8; i++){
        task_sleep(0);
        uint64_t b = read_tsc();
        seed[i * 4 + 0] = (uint8_t)(b);
        seed[i * 4 + 1] = (uint8_t)(b >> 8);
        seed[i * 4 + 2] = (uint8_t)(b >> 16) ^ (uint8_t)ticks;
        seed[i * 4 + 3] = (uint8_t)(b >> 24) ^ (uint8_t)(a >> (i * 3));
        a = b;
    }
    seed[0] ^= t.sec;
    seed[1] ^= t.min;
    seed[2] ^= t.hour;
    seed[3] ^= (uint8_t)t.year;

    br_ssl_engine_inject_entropy(&cc->eng, seed, sizeof(seed));
}

// ------------------------------------------------------ transport callbacks

static int sock_read(void* ctx, unsigned char* buf, size_t len){
    struct tls_conn* t = (struct tls_conn*)ctx;
    unsigned long long deadline = ticks + IO_TIMEOUT_MS / 10;

    for (;;){
        int n = tcp_read(t->tcp, buf, (uint32_t)len);
        if (n > 0) return n;
        if (n < 0){ t->eof = 1; return -1; }
        if (ticks > deadline) return -1;
        task_sleep(10);
    }
}

static int sock_write(void* ctx, const unsigned char* buf, size_t len){
    struct tls_conn* t = (struct tls_conn*)ctx;
    int n = tcp_write(t->tcp, buf, (uint32_t)len);
    return n > 0 ? n : -1;
}

// ------------------------------------------------------------------- API

tls_conn_t* tls_client_open(tcp_conn_t* tcp, const char* host){
    last_error[0] = 0;

    struct tls_conn* t = (struct tls_conn*)kmalloc(sizeof(struct tls_conn));
    if (!t){ strcpy(last_error, "out of memory"); return 0; }
    memset(t, 0, sizeof(*t));
    t->tcp = tcp;

    t->iobuf = (unsigned char*)kmalloc(BR_SSL_BUFSIZE_BIDI);
    if (!t->iobuf){ strcpy(last_error, "out of memory for TLS buffers"); kfree(t); return 0; }

    // init_full wires up every hash, cipher and key-exchange implementation
    // BearSSL supports, then we swap the certificate validator for our own.
    // Doing it this way means we inherit the library's algorithm choices
    // rather than hand-assembling a profile and getting it subtly wrong.
    br_ssl_client_init_full(&t->cc, &t->xc_minimal, 0, 0);
    t->xc.vtable = &NOCHECK_VTABLE;
    br_ssl_engine_set_x509(&t->cc.eng, &t->xc.vtable);

    br_ssl_engine_set_buffer(&t->cc.eng, t->iobuf, BR_SSL_BUFSIZE_BIDI, 1);
    seed_entropy(&t->cc);

    if (!br_ssl_client_reset(&t->cc, host, 0)){
        strcpy(last_error, "could not start TLS handshake");
        kfree(t->iobuf); kfree(t);
        return 0;
    }

    br_sslio_init(&t->io, &t->cc.eng, sock_read, t, sock_write, t);

    // Force the handshake to run now rather than on first use, so a failure
    // is reported as "TLS handshake failed" instead of as a mysterious empty
    // response later on.
    if (br_sslio_flush(&t->io) < 0){
        int err = br_ssl_engine_last_error(&t->cc.eng);
        ksnprintf(last_error, sizeof(last_error), "TLS handshake failed, BearSSL error %d", err);
        kfree(t->iobuf); kfree(t);
        return 0;
    }

    return t;
}

int tls_write(tls_conn_t* t, const void* data, uint32_t len){
    if (!t) return -1;
    if (br_sslio_write_all(&t->io, data, len) < 0) return -1;
    if (br_sslio_flush(&t->io) < 0) return -1;
    return (int)len;
}

int tls_read(tls_conn_t* t, uint8_t* buf, uint32_t cap){
    if (!t) return -1;
    if (t->eof) return -1;

    int n = br_sslio_read(&t->io, buf, cap);
    if (n <= 0){
        // A clean TLS close_notify and a dropped connection both end the
        // stream; the caller only needs to know there is no more body.
        t->eof = 1;
        return -1;
    }
    return n;
}

void tls_close(tls_conn_t* t){
    if (!t) return;

    // Only negotiate a shutdown if the stream is still live. Once the peer
    // has closed, br_sslio_close() has nothing left to drive it: the engine
    // wants neither to send nor receive a record, so its run-until loop spins
    // without ever reaching the closed state. Skipping it costs nothing --
    // the connection is already finished -- and removes the hang.
    if (!t->eof) br_sslio_close(&t->io);

    if (t->iobuf) kfree(t->iobuf);
    kfree(t);
}
