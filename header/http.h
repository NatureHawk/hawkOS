#pragma once
#include <stdint.h>
#include "header/net.h"

#define URL_MAX  512
#define HOST_MAX 128

typedef struct {
    int      https;
    char     host[HOST_MAX];
    uint16_t port;
    char     path[URL_MAX];
} url_t;

// Splits an absolute or scheme-less URL. A missing scheme is treated as
// http, and a missing path as "/".
int url_parse(const char* text, url_t* out);

// Resolves `ref` against `base` and writes the absolute result to out.
// Handles absolute URLs, protocol-relative ("//host/x"), root-relative
// ("/x") and same-directory references.
void url_resolve(const url_t* base, const char* ref, char* out, uint32_t cap);

// Percent-decoding, and pulling a real destination out of a search
// engine's redirector link. See the note in http.c.
int url_percent_decode(const char* s, char* out, uint32_t cap);
int url_unwrap(const char* url, char* out, uint32_t cap);

typedef struct {
    int      status;              // HTTP status code, or 0 if the request failed
    char     error[96];           // human-readable reason when status is 0
    char     content_type[64];
    char     final_url[URL_MAX];  // after any redirects
    uint8_t* body;                // heap-allocated, NUL-terminated
    uint32_t body_len;
} http_response_t;

// Progress callback so the UI can say what is happening during the seconds
// a fetch takes; may be NULL.
typedef void (*http_progress_t)(void* ctx, const char* stage);

// Performs a GET, following up to 5 redirects. Blocks the calling task, so
// run it on a task of its own if a UI needs to stay responsive.
int  http_get(const char* url, http_response_t* out, http_progress_t cb, void* ctx);
void http_response_free(http_response_t* r);

// Streaming. For a body too long to hold, or one the caller wants to start on
// before it has all arrived. http_open follows redirects and reads the
// headers; http_read then returns >0 bytes, 0 for "nothing yet, try again",
// or -1 at the end of the body. Chunked transfer coding is undone on the way.
typedef struct http_stream http_stream_t;

http_stream_t* http_open(const char* url, http_progress_t cb, void* ctx,
                         char* err, uint32_t errcap);
int            http_status(const http_stream_t* s);
const char*    http_content_type(const http_stream_t* s);
int32_t        http_content_length(const http_stream_t* s);   // -1 if unknown
// Copies the value of response header `name` (case-insensitive). 0 if present.
int            http_header(const http_stream_t* s, const char* name, char* out, uint32_t cap);
int            http_read(http_stream_t* s, uint8_t* buf, uint32_t cap);
// Blocks until the stream has bytes to read (or has ended), or timeout_ms
// passes. Use it instead of sleeping when http_read returned 0.
void           http_wait(http_stream_t* s, uint32_t timeout_ms);
void           http_close(http_stream_t* s);
