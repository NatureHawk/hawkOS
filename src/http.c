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
    if (ref[0] == '/'){                                  // root-relative
        ksnprintf(out, cap, "%s%s%s", scheme, base->host, ref);
        return;
    }

    // Same directory: keep everything up to and including the last slash.
    char dir[URL_MAX];
    strncpy(dir, base->path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = 0;
    char* slash = dir;
    char* last = dir;
    for (; *slash; slash++) if (*slash == '/') last = slash;
    last[1] = 0;

    ksnprintf(out, cap, "%s%s%s%s", scheme, base->host, dir, ref);
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

static int fetch_once(const url_t* u, http_response_t* r,
                      http_progress_t cb, void* ctx,
                      char* redirect_out, uint32_t redirect_cap){
    char msg[128];

    ipv4_t ip;
    ksnprintf(msg, sizeof(msg), "Resolving %s", u->host);
    report(cb, ctx, msg);
    if (dns_resolve(u->host, &ip, 6000) != 0){
        ksnprintf(r->error, sizeof(r->error), "cannot resolve %s", u->host);
        return -1;
    }

    char ipstr[16];
    ksnprintf(msg, sizeof(msg), "Connecting to %s:%u", net_ip_str(ip, ipstr), u->port);
    report(cb, ctx, msg);

    tcp_conn_t* c = tcp_open(ip, u->port);
    if (!c){ strcpy(r->error, "no free connection"); return -1; }

    unsigned long long deadline = ticks + 800;                 // 8 s to connect
    while (tcp_state(c) == TCP_SYN_SENT && ticks < deadline) task_sleep(10);

    if (tcp_state(c) != TCP_ESTABLISHED){
        strcpy(r->error, "connection refused or timed out");
        tcp_free(c);
        return -1;
    }

    tls_conn_t* tls = 0;
    if (u->https){
        report(cb, ctx, "TLS handshake");
        tls = tls_client_open(c, u->host);
        if (!tls){
            ksnprintf(r->error, sizeof(r->error), "%s", tls_last_error());
            tcp_close(c); tcp_free(c);
            return -1;
        }
    }

    // Identify honestly, ask for an unencoded body, and close after one
    // response: without Connection: close a server may hold the socket open
    // and this client has no keep-alive logic to take advantage of it.
    char req[1024];
    int reqlen = ksnprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: hawkOS/0.6\r\n"
        "Accept: text/html,text/plain,*/*\r\n"
        "Accept-Encoding: identity\r\n"
        "Connection: close\r\n"
        "\r\n", u->path, u->host);

    report(cb, ctx, "Sending request");
    int wrote = tls ? tls_write(tls, req, (uint32_t)reqlen)
                    : tcp_write(c, req, (uint32_t)reqlen);
    if (wrote < 0){
        strcpy(r->error, "could not send request");
        if (tls) tls_close(tls);
        tcp_close(c); tcp_free(c);
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
            if (ticks - last_data > STALL_MS / 10) done = 1;
            task_sleep(10);
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
