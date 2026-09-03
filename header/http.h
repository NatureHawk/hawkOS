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
