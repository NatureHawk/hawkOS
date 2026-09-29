// src/http.c — HTTP/1.1 client over the kernel's TCP
//
// Blocking by design. Making it asynchronous would mean threading a state
// machine through every caller; instead the browser runs each fetch on its
// own task, and the scheduler keeps the desktop responsive for free. That is
// the whole reason the multitasking layer was built first.
#include <stdint.h>
#include "header/http.h"
#include "header/net.h"
#include "header/netcfg.h"
#include "header/tcp.h"
#include "header/kheap.h"
#include "header/kstring.h"
#include "header/task.h"
#include "header/sync.h"
#include "header/tls.h"
#include "header/kprintf.h"

extern volatile unsigned long long ticks;

// 1 MB. A Wikipedia article's raw HTML runs to several hundred KB before
// any of it is thrown away, and truncating mid-document produces a page that
// looks broken rather than one that looks large.
#define BODY_CAP      (1024u * 1024u)
#define HEADER_CAP    8192u
#define MAX_REDIRECTS 5
#define STALL_MS      12000
#define FIRST_BYTE_MS 30000

// ------------------------------------------------------------------- URLs

int url_parse(const char* text, url_t* out){
    if (!text || !*text) return -1;

    memset(out, 0, sizeof(*out));
    out->https = 0;
    out->port  = 80;

    const char* p = text;
    if (kstrnicmp(p, "http://", 7) == 0)       { p += 7; }
    else if (kstrnicmp(p, "https://", 8) == 0) { p += 8; out->https = 1; out->port = 443; }
    else if (strstr(p, "://"))                 { return -1; }   // some other scheme

    // Host runs to the first '/', ':' or end.
    uint32_t i = 0;
    while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#'){
        if (i < HOST_MAX - 1) out->host[i++] = *p;
        p++;
    }
    out->host[i] = 0;
    if (!out->host[0]) return -1;

    if (*p == ':'){
        p++;
        uint32_t port = 0;
        while (*p >= '0' && *p <= '9'){ port = port * 10 + (uint32_t)(*p - '0'); p++; }
        if (port == 0 || port > 65535) return -1;
        out->port = (uint16_t)port;
    }

    if (*p == '#' || !*p){
        strcpy(out->path, "/");
    } else {
        uint32_t j = 0;
        // A fragment is a client-side concept; it never goes on the wire.
        while (*p && *p != '#' && j < URL_MAX - 1) out->path[j++] = *p++;
        out->path[j] = 0;
        if (out->path[0] != '/'){
            memmove(out->path + 1, out->path, j + 1);
            out->path[0] = '/';
        }
    }
    return 0;
}

void url_resolve(const url_t* base, const char* ref, char* out, uint32_t cap){
    if (!ref || !*ref){ out[0] = 0; return; }

    if (kstrnicmp(ref, "http://", 7) == 0 || kstrnicmp(ref, "https://", 8) == 0){
        strncpy(out, ref, cap - 1); out[cap - 1] = 0;
        return;
    }

    const char* scheme = base->https ? "https://" : "http://";

    if (ref[0] == '/' && ref[1] == '/'){                 // protocol-relative
        ksnprintf(out, cap, "%s%s", base->https ? "https:" : "http:", ref);
        return;
    }

    // The authority carries the port whenever it is not the scheme's default.
    // Dropping it sent every root-relative link on a page served from
    // host:8090 to port 80 on the same host, where nothing was listening.
    char host[HOST_MAX + 8];
    if (base->port != (base->https ? 443 : 80))
        ksnprintf(host, sizeof(host), "%s:%u", base->host, base->port);
    else
        ksnprintf(host, sizeof(host), "%s", base->host);

    if (ref[0] == '/'){                                  // root-relative
        ksnprintf(out, cap, "%s%s%s", scheme, host, ref);
        return;
    }

    // A bare query replaces the query of the current path, not the file name
    // -- "?page=2" on /results?page=1 is /results?page=2.
    if (ref[0] == '?'){
        char path[URL_MAX];
        strncpy(path, base->path, sizeof(path) - 1);
        path[sizeof(path) - 1] = 0;
        char* q = strchr(path, '?');
        if (q) *q = 0;
        ksnprintf(out, cap, "%s%s%s%s", scheme, host, path, ref);
        return;
    }

    // Same directory: keep everything up to and including the last slash.
    char dir[URL_MAX];
    strncpy(dir, base->path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = 0;
    char* qm = strchr(dir, '?');                 // a slash in the query is not a directory
    if (qm) *qm = 0;
    char* slash = dir;
    char* last = dir;
    for (; *slash; slash++) if (*slash == '/') last = slash;
    last[1] = 0;

    ksnprintf(out, cap, "%s%s%s%s", scheme, host, dir, ref);
}

// --------------------------------------------------------------- fetching

static void report(http_progress_t cb, void* ctx, const char* stage){
    if (cb) cb(ctx, stage);
}

static int header_int(const char* headers, const char* name){
    const char* p = headers;
    uint32_t nlen = strlen(name);
    while (*p){
        if (kstrnicmp(p, name, nlen) == 0){
            p += nlen;
            while (*p == ' ' || *p == ':') p++;
            int v = 0;
            while (*p >= '0' && *p <= '9'){ v = v * 10 + (*p - '0'); p++; }
            return v;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return -1;
}

static void header_str(const char* headers, const char* name, char* out, uint32_t cap){
    out[0] = 0;
    const char* p = headers;
    uint32_t nlen = strlen(name);
    while (*p){
        if (kstrnicmp(p, name, nlen) == 0){
            p += nlen;
            while (*p == ' ' || *p == ':') p++;
            uint32_t i = 0;
            while (*p && *p != '\r' && *p != '\n' && i < cap - 1) out[i++] = *p++;
            out[i] = 0;
            return;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
}

// Chunked transfer encoding: a hex length, CRLF, that many bytes, CRLF,
// repeating until a zero-length chunk. Servers pick it whenever they stream
// a response, so a client that cannot decode it sees garbage on a large
// fraction of the web.
static uint32_t dechunk(uint8_t* buf, uint32_t len){
    uint32_t in = 0, out = 0;
    while (in < len){
        uint32_t size = 0;
        int digits = 0;
        while (in < len){
            uint8_t c = buf[in];
            int v;
            if      (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else break;
            size = size * 16 + (uint32_t)v;
            in++; digits++;
        }
        if (!digits) break;

        while (in < len && buf[in] != '\n') in++;     // skip any chunk extension
        if (in < len) in++;

        if (size == 0) break;
        if (in + size > len) size = len - in;

        memmove(buf + out, buf + in, size);
        out += size;
        in  += size;

        while (in < len && (buf[in] == '\r' || buf[in] == '\n')) in++;
    }
    return out;
}

// Resolves, connects and (for https) runs the handshake. On failure writes a
// reason into err and returns -1 with nothing left open.
static int open_conn(const url_t* u, tcp_conn_t** pc, tls_conn_t** pt,
                     char* err, uint32_t errcap, http_progress_t cb, void* ctx){
    char msg[128];
    *pc = 0; *pt = 0;

    ipv4_t ip;
    ksnprintf(msg, sizeof(msg), "Resolving %s", u->host);
    report(cb, ctx, msg);
    if (dns_resolve(u->host, &ip, 6000) != 0){
        ksnprintf(err, errcap, "cannot resolve %s", u->host);
        return -1;
    }

    char ipstr[16];
    ksnprintf(msg, sizeof(msg), "Connecting to %s:%u", net_ip_str(ip, ipstr), u->port);
    report(cb, ctx, msg);

    tcp_conn_t* c = tcp_open(ip, u->port);
    if (!c){ ksnprintf(err, errcap, "cannot open a connection: %s", tcp_open_error()); return -1; }

    tcp_wait_connect(c, 8000);                                 // 8 s to connect

    if (tcp_state(c) != TCP_ESTABLISHED){
        ksnprintf(err, errcap, "connection refused or timed out");
        tcp_free(c);
        return -1;
    }

    tls_conn_t* tls = 0;
    if (u->https){
        report(cb, ctx, "TLS handshake");
        tls = tls_client_open(c, u->host);
        if (!tls){
            ksnprintf(err, errcap, "%s", tls_last_error());
            tcp_close(c); tcp_free(c);
            return -1;
        }
    }
    *pc = c; *pt = tls;
    return 0;
}

static void close_conn(tcp_conn_t* c, tls_conn_t* tls){
    if (tls) tls_close(tls);
    if (c){ tcp_close(c); tcp_free(c); }
}

// Identify honestly, ask for an unencoded body, and close after one
// response: without Connection: close a server may hold the socket open
// and this client has no keep-alive logic to take advantage of it.
static int send_get(const url_t* u, tcp_conn_t* c, tls_conn_t* tls){
    char host[HOST_MAX + 8];
    if (u->port != (u->https ? 443 : 80)) ksnprintf(host, sizeof(host), "%s:%u", u->host, u->port);
    else                                  ksnprintf(host, sizeof(host), "%s", u->host);

    char req[1024];
    int reqlen = ksnprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: hawkOS/0.9\r\n"
        "Accept: text/html,text/plain,image/png,image/jpeg,image/gif,*/*\r\n"
        "Accept-Encoding: identity\r\n"
        "Connection: close\r\n"
        "\r\n", u->path, host);

    return tls ? tls_write(tls, req, (uint32_t)reqlen)
               : tcp_write(c, req, (uint32_t)reqlen);
}

static int fetch_once(const url_t* u, http_response_t* r,
                      http_progress_t cb, void* ctx,
                      char* redirect_out, uint32_t redirect_cap){
    char msg[128];

    tcp_conn_t* c;
    tls_conn_t* tls;
    if (open_conn(u, &c, &tls, r->error, sizeof(r->error), cb, ctx) != 0) return -1;

    report(cb, ctx, "Sending request");
    if (send_get(u, c, tls) < 0){
        strcpy(r->error, "could not send request");
        close_conn(c, tls);
        return -1;
    }

    uint8_t* buf = (uint8_t*)kmalloc(BODY_CAP + HEADER_CAP + 1);
    if (!buf){
        strcpy(r->error, "out of memory");
        if (tls) tls_close(tls);
        tcp_close(c); tcp_free(c);
        return -1;
    }

    report(cb, ctx, "Receiving");

    uint32_t total = 0;
    unsigned long long last_data = ticks;
    int done = 0;

    while (!done){
        int n = tls ? tls_read(tls, buf + total, BODY_CAP + HEADER_CAP - total)
                    : tcp_read(c, buf + total, BODY_CAP + HEADER_CAP - total);
        if (n > 0){
            total += (uint32_t)n;
            last_data = ticks;
            if (total >= BODY_CAP + HEADER_CAP) break;
            if ((total & 0x3FFF) == 0){
                ksnprintf(msg, sizeof(msg), "Receiving %u KB", total / 1024u);
                report(cb, ctx, msg);
            }
        } else if (n < 0){
            done = 1;                                   // peer closed
        } else {
            // Twelve seconds of silence mid-response is a dead server. Before
            // the first byte it may just be a slow one -- a gateway that has
            // to ask YouTube before it can answer -- so that wait is longer.
            if (ticks - last_data > (total ? STALL_MS : FIRST_BYTE_MS) / 10) done = 1;
            else tcp_wait_data(c, 100);          // woken when bytes arrive
        }
    }
    buf[total] = 0;

    if (tls) tls_close(tls);
    tcp_close(c);
    tcp_free(c);

    if (total == 0){
        strcpy(r->error, "empty response");
        kfree(buf);
        return -1;
    }

    // Split headers from body at the blank line.
    uint32_t hdr_end = 0;
    for (uint32_t i = 0; i + 3 < total; i++){
        if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n'){
            hdr_end = i + 4; break;
        }
        if (buf[i] == '\n' && buf[i+1] == '\n'){ hdr_end = i + 2; break; }
    }
    if (!hdr_end){
        strcpy(r->error, "malformed response (no header terminator)");
        kfree(buf);
        return -1;
    }


    // On the heap, not the stack: 8 KB of locals is more than a task stack
    // should carry, especially under a TLS handshake that is already deep.
    char* headers = (char*)kmalloc(HEADER_CAP);
    if (!headers){ strcpy(r->error, "out of memory"); kfree(buf); return -1; }
    uint32_t hlen = hdr_end < HEADER_CAP - 1 ? hdr_end : HEADER_CAP - 1;
    memcpy(headers, buf, hlen);
    headers[hlen] = 0;

    r->status = 0;
    if (kstrnicmp(headers, "HTTP/", 5) == 0){
        const char* sp = strchr(headers, ' ');
        if (sp) r->status = (int)(sp[1] - '0') * 100 + (int)(sp[2] - '0') * 10 + (int)(sp[3] - '0');
    }

    if (r->status >= 300 && r->status < 400 && redirect_out){
        char loc[URL_MAX];
        header_str(headers, "Location", loc, sizeof(loc));
        if (loc[0]){
            url_resolve(u, loc, redirect_out, redirect_cap);
            kfree(headers);
            kfree(buf);
            return 1;                                   // caller should follow
        }
    }

    header_str(headers, "Content-Type", r->content_type, sizeof(r->content_type));

    uint32_t body_len = total - hdr_end;
    memmove(buf, buf + hdr_end, body_len);

    char te[32];
    header_str(headers, "Transfer-Encoding", te, sizeof(te));
    if (te[0] && strstr(te, "chunked")) body_len = dechunk(buf, body_len);

    int cl = header_int(headers, "Content-Length");
    if (cl > 0 && (uint32_t)cl < body_len) body_len = (uint32_t)cl;

    buf[body_len] = 0;
    r->body     = buf;
    r->body_len = body_len;
    kfree(headers);
    return 0;
}

int http_get(const char* url, http_response_t* out, http_progress_t cb, void* ctx){
    memset(out, 0, sizeof(*out));

    if (!net_configured()){
        strcpy(out->error, "network is not configured");
        return -1;
    }

    char current[URL_MAX];
    strncpy(current, url, sizeof(current) - 1);
    current[sizeof(current) - 1] = 0;

    for (int hop = 0; hop <= MAX_REDIRECTS; hop++){
        url_t u;
        if (url_parse(current, &u) != 0){
            ksnprintf(out->error, sizeof(out->error), "cannot parse URL: %s", current);
            return -1;
        }

        if (u.https && !tls_available()){
            ksnprintf(out->error, sizeof(out->error),
                      "https is not supported in this build (%s)", u.host);
            return -1;
        }

        strncpy(out->final_url, current, sizeof(out->final_url) - 1);
        out->final_url[sizeof(out->final_url) - 1] = 0;

        char next[URL_MAX];
        next[0] = 0;
        int rc = fetch_once(&u, out, cb, ctx, next, sizeof(next));

        if (rc == 0) return 0;
        if (rc < 0)  return -1;

        // rc == 1: a redirect we should follow.
        strncpy(current, next, sizeof(current) - 1);
        current[sizeof(current) - 1] = 0;
    }

    strcpy(out->error, "too many redirects");
    return -1;
}

void http_response_free(http_response_t* r){
    if (r->body) kfree(r->body);
    r->body = 0;
    r->body_len = 0;
}

// ------------------------------------------------------------- streaming
//
// For responses that should not, or cannot, be held whole: a video that runs
// for minutes, or anything whose end is simply "when the server stops". The
// caller pulls the body in pieces at its own pace. Headers are read and
// redirects followed inside http_open, so what comes back is either a stream
// positioned at the first body byte or nothing.

#define RAW_CAP (32u * 1024u)

struct http_stream {
    tcp_conn_t* tcp;
    tls_conn_t* tls;
    int         status;
    char        content_type[64];
    int32_t     content_length;     // -1 when the server did not say
    uint32_t    delivered;
    char        final_url[URL_MAX];
    char        headers[2048];      // the response headers, for http_header

    // Bytes off the wire not yet handed out. Header parsing reads past the
    // blank line, and chunk framing has to be peeled off in place, so reads
    // go through this rather than straight into the caller's buffer.
    uint8_t     raw[RAW_CAP];
    uint32_t    raw_off, raw_len;

    int         chunked;
    int         chunk_state;        // 0 size line, 1 data, 2 CRLF after data
    uint32_t    chunk_left;
    int         chunk_digits, chunk_ext;
    int         eof;
};

static int stream_fill(http_stream_t* s){
    if (s->raw_off < s->raw_len) return (int)(s->raw_len - s->raw_off);
    s->raw_off = s->raw_len = 0;
    int n = s->tls ? tls_read(s->tls, s->raw, RAW_CAP)
                   : tcp_read(s->tcp, s->raw, RAW_CAP);
    if (n > 0) s->raw_len = (uint32_t)n;
    return n;
}

// Reads the status line and headers into `hdr` (NUL-terminated), leaving
// whatever followed them in the raw buffer. -1 on timeout or close.
static int stream_headers(http_stream_t* s, char* hdr, uint32_t cap){
    uint32_t n = 0;
    unsigned long long last = ticks;
    // A proxy that has to fetch and start transcoding before it can answer
    // takes longer than a web server, so the wait here is generous.
    while (ticks - last < 3000){
        int r = stream_fill(s);
        if (r < 0) return -1;
        if (r == 0){ tcp_wait_data(s->tcp, 100); continue; }
        last = ticks;
        while (s->raw_off < s->raw_len){
            char c = (char)s->raw[s->raw_off++];
            if (n < cap - 1) hdr[n++] = c;
            hdr[n] = 0;
            if (n >= 4 && hdr[n-1] == '\n' && hdr[n-2] == '\r' && hdr[n-3] == '\n' && hdr[n-4] == '\r')
                return 0;
            if (n >= 2 && hdr[n-1] == '\n' && hdr[n-2] == '\n') return 0;
        }
    }
    return -1;
}

http_stream_t* http_open(const char* url, http_progress_t cb, void* ctx,
                         char* err, uint32_t errcap){
    char current[URL_MAX];
    strncpy(current, url, sizeof(current) - 1);
    current[sizeof(current) - 1] = 0;
    char dummy[8];
    if (!err || !errcap){ err = dummy; errcap = sizeof(dummy); }
    err[0] = 0;

    if (!net_configured()){ ksnprintf(err, errcap, "network is not configured"); return 0; }

    char* hdr = (char*)kmalloc(HEADER_CAP);
    if (!hdr){ ksnprintf(err, errcap, "out of memory"); return 0; }

    for (int hop = 0; hop <= MAX_REDIRECTS; hop++){
        url_t u;
        if (url_parse(current, &u) != 0){
            ksnprintf(err, errcap, "cannot parse URL: %s", current);
            break;
        }
        if (u.https && !tls_available()){
            ksnprintf(err, errcap, "https is not supported in this build");
            break;
        }

        http_stream_t* s = (http_stream_t*)kmalloc(sizeof(http_stream_t));
        if (!s){ ksnprintf(err, errcap, "out of memory"); break; }
        memset(s, 0, sizeof(*s));
        s->content_length = -1;
        strncpy(s->final_url, current, sizeof(s->final_url) - 1);

        if (open_conn(&u, &s->tcp, &s->tls, err, errcap, cb, ctx) != 0){ kfree(s); break; }
        report(cb, ctx, "Sending request");
        if (send_get(&u, s->tcp, s->tls) < 0){
            ksnprintf(err, errcap, "could not send request");
            http_close(s);
            break;
        }
        report(cb, ctx, "Waiting for response");
        if (stream_headers(s, hdr, HEADER_CAP) != 0){
            ksnprintf(err, errcap, "no response from server");
            http_close(s);
            break;
        }

        if (kstrnicmp(hdr, "HTTP/", 5) == 0){
            const char* sp = strchr(hdr, ' ');
            if (sp) s->status = (sp[1] - '0') * 100 + (sp[2] - '0') * 10 + (sp[3] - '0');
        }

        if (s->status >= 300 && s->status < 400){
            char loc[URL_MAX];
            header_str(hdr, "Location", loc, sizeof(loc));
            if (loc[0]){
                char next[URL_MAX];
                url_resolve(&u, loc, next, sizeof(next));
                strncpy(current, next, sizeof(current) - 1);
                current[sizeof(current) - 1] = 0;
                http_close(s);
                continue;
            }
        }

        header_str(hdr, "Content-Type", s->content_type, sizeof(s->content_type));
        strncpy(s->headers, hdr, sizeof(s->headers) - 1);
        s->headers[sizeof(s->headers) - 1] = 0;
        char te[32];
        header_str(hdr, "Transfer-Encoding", te, sizeof(te));
        s->chunked = te[0] && strstr(te, "chunked");
        char cl[16];
        header_str(hdr, "Content-Length", cl, sizeof(cl));
        if (cl[0] && !s->chunked) s->content_length = header_int(hdr, "Content-Length");

        kfree(hdr);
        return s;
    }

    if (!err[0]) ksnprintf(err, errcap, "too many redirects");
    kfree(hdr);
    return 0;
}

int http_status(const http_stream_t* s){ return s ? s->status : 0; }

int http_header(const http_stream_t* s, const char* name, char* out, uint32_t cap){
    if (!s || !cap) return -1;
    header_str(s->headers, name, out, cap);
    return out[0] ? 0 : -1;
}
const char* http_content_type(const http_stream_t* s){ return s ? s->content_type : ""; }
int32_t http_content_length(const http_stream_t* s){ return s ? s->content_length : -1; }

static int hexdig(uint8_t c){
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int http_read(http_stream_t* s, uint8_t* buf, uint32_t cap){
    if (!s || s->eof) return -1;
    if (s->content_length >= 0 && s->delivered >= (uint32_t)s->content_length){
        s->eof = 1;
        return -1;
    }

    int r = stream_fill(s);
    if (r < 0){ s->eof = 1; return -1; }
    if (r == 0) return 0;

    if (!s->chunked){
        uint32_t n = s->raw_len - s->raw_off;
        if (n > cap) n = cap;
        if (s->content_length >= 0 && n > (uint32_t)s->content_length - s->delivered)
            n = (uint32_t)s->content_length - s->delivered;
        memcpy(buf, s->raw + s->raw_off, n);
        s->raw_off   += n;
        s->delivered += n;
        return (int)n;
    }

    uint32_t out = 0;
    while (s->raw_off < s->raw_len && out < cap){
        uint8_t c = s->raw[s->raw_off];
        if (s->chunk_state == 0){
            s->raw_off++;
            int v = hexdig(c);
            if (c == '\n'){
                if (!s->chunk_digits) continue;            // blank line between chunks
                if (s->chunk_left == 0){ s->eof = 1; break; }
                s->chunk_state = 1;
            } else if (c == ';'){
                s->chunk_ext = 1;
            } else if (v >= 0 && !s->chunk_ext){
                s->chunk_left = s->chunk_left * 16 + (uint32_t)v;
                s->chunk_digits++;
            }
        } else if (s->chunk_state == 1){
            uint32_t n = s->raw_len - s->raw_off;
            if (n > s->chunk_left) n = s->chunk_left;
            if (n > cap - out)     n = cap - out;
            memcpy(buf + out, s->raw + s->raw_off, n);
            s->raw_off    += n;
            s->chunk_left -= n;
            out           += n;
            if (s->chunk_left == 0) s->chunk_state = 2;
        } else {
            s->raw_off++;
            if (c == '\n'){
                s->chunk_state  = 0;
                s->chunk_digits = 0;
                s->chunk_ext    = 0;
                s->chunk_left   = 0;
            }
        }
    }
    s->delivered += out;
    if (out == 0 && s->eof) return -1;
    return (int)out;
}

void http_wait(http_stream_t* s, uint32_t timeout_ms){
    if (!s) return;
    if (s->raw_off < s->raw_len) return;          // already buffered
    tcp_wait_data(s->tcp, timeout_ms);
}

void http_close(http_stream_t* s){
    if (!s) return;
    close_conn(s->tcp, s->tls);
    kfree(s);
}

// --------------------------------------------------- redirect unwrapping

static int hexval(char c){
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int url_percent_decode(const char* s, char* out, uint32_t cap){
    uint32_t o = 0;
    for (const char* p = s; *p && o + 1 < cap; p++){
        if (*p == '%'){
            int hi = hexval(p[1]);
            int lo = hi >= 0 ? hexval(p[2]) : -1;
            if (lo >= 0){ out[o++] = (char)(hi * 16 + lo); p += 2; continue; }
        }
        // Inside a query string '+' is an encoded space; a literal plus
        // would have arrived as %2B.
        out[o++] = (*p == '+') ? ' ' : *p;
    }
    out[o] = 0;
    return 0;
}

// Search engines route results through their own redirector, with the real
// destination sitting percent-encoded in a query parameter. Following the
// redirector costs an extra request and often lands on a page that only
// redirects via JavaScript, which this browser cannot run - so pull the
// destination out and go straight there instead.
int url_unwrap(const char* url, char* out, uint32_t cap){
    static const char* const KEYS[] = { "uddg=", "url=", "u=", "q=" };

    for (int k = 0; k < 4; k++){
        const char* p = strstr(url, KEYS[k]);
        if (!p) continue;
        if (p != url && p[-1] != '?' && p[-1] != '&') continue;

        p += strlen(KEYS[k]);

        char enc[URL_MAX];
        uint32_t n = 0;
        while (p[n] && p[n] != '&' && n < sizeof(enc) - 1) n++;
        memcpy(enc, p, n);
        enc[n] = 0;

        char dec[URL_MAX];
        url_percent_decode(enc, dec, sizeof(dec));

        // Only treat it as a redirect if what came out is itself a URL;
        // otherwise this was an ordinary query parameter.
        if (kstrnicmp(dec, "http://", 7) == 0 || kstrnicmp(dec, "https://", 8) == 0){
            strncpy(out, dec, cap - 1);
            out[cap - 1] = 0;
            return 0;
        }
    }
    return -1;
}
