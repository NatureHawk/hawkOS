#include "header/kstring.h"

void* memset(void* dst, int c, size_t n){
    uint8_t* d = (uint8_t*)dst;
    uint8_t  v = (uint8_t)c;

    // Fill the aligned middle a word at a time; a byte loop over a 4 MB
    // framebuffer clear is four times the work for no reason.
    while (n && ((uintptr_t)d & 3u)) { *d++ = v; n--; }
    uint32_t w = ((uint32_t)v << 24) | ((uint32_t)v << 16) | ((uint32_t)v << 8) | v;
    uint32_t* dw = (uint32_t*)d;
    while (n >= 4) { *dw++ = w; n -= 4; }
    d = (uint8_t*)dw;
    while (n--) *d++ = v;
    return dst;
}

void* memcpy(void* dst, const void* src, size_t n){
    uint8_t*       d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    if (!(((uintptr_t)d | (uintptr_t)s) & 3u)) {
        uint32_t*       dw = (uint32_t*)d;
        const uint32_t* sw = (const uint32_t*)s;
        while (n >= 4) { *dw++ = *sw++; n -= 4; }
        d = (uint8_t*)dw; s = (const uint8_t*)sw;
    }
    while (n--) *d++ = *s++;
    return dst;
}

void* memmove(void* dst, const void* src, size_t n){
    uint8_t*       d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    if (d == s || n == 0) return dst;
    if (d < s) return memcpy(dst, src, n);
    d += n; s += n;                       // overlapping forwards: copy backwards
    while (n--) *--d = *--s;
    return dst;
}

int memcmp(const void* a, const void* b, size_t n){
    const uint8_t* x = (const uint8_t*)a;
    const uint8_t* y = (const uint8_t*)b;
    for (size_t i = 0; i < n; i++) if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    return 0;
}

size_t strlen(const char* s){ size_t n = 0; while (s[n]) n++; return n; }

int strcmp(const char* a, const char* b){
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char* a, const char* b, size_t n){
    for (size_t i = 0; i < n; i++){
        if (a[i] != b[i]) return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

char* strcpy(char* dst, const char* src){
    char* p = dst;
    while ((*p++ = *src++)) { }
    return dst;
}

char* strncpy(char* dst, const char* src, size_t n){
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}

char* strchr(const char* s, int c){
    for (; *s; s++) if (*s == (char)c) return (char*)s;
    return (c == 0) ? (char*)s : 0;
}

char* strstr(const char* hay, const char* needle){
    if (!*needle) return (char*)hay;
    for (; *hay; hay++){
        const char* h = hay;
        const char* n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char*)hay;
    }
    return 0;
}

static char lower(char c){ return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

int kstricmp(const char* a, const char* b){
    while (*a && lower(*a) == lower(*b)) { a++; b++; }
    return (int)(unsigned char)lower(*a) - (int)(unsigned char)lower(*b);
}

int kstrnicmp(const char* a, const char* b, size_t n){
    for (size_t i = 0; i < n; i++){
        char x = lower(a[i]), y = lower(b[i]);
        if (x != y) return (int)(unsigned char)x - (int)(unsigned char)y;
        if (!x) return 0;
    }
    return 0;
}

// ------------------------------------------------------------- formatting

typedef struct { char* buf; size_t cap; size_t len; } sink_t;

static void s_putc(sink_t* s, char c){
    if (s->len + 1 < s->cap) s->buf[s->len] = c;
    s->len++;
}

static void s_puts(sink_t* s, const char* p){ while (*p) s_putc(s, *p++); }

static void s_putu(sink_t* s, uint32_t v, uint32_t base, int upper, int width, char pad){
    char tmp[33];
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v) { tmp[i++] = digits[v % base]; v /= base; }
    while (i < width) tmp[i++] = pad;
    while (i--) s_putc(s, tmp[i]);
}

int kvsnprintf(char* buf, size_t cap, const char* fmt, va_list ap){
    sink_t s = { buf, cap, 0 };

    for (; *fmt; fmt++){
        if (*fmt != '%') { s_putc(&s, *fmt); continue; }
        fmt++;

        char pad = ' ';
        int  width = 0;
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }

        switch (*fmt){
            case '%': s_putc(&s, '%'); break;
            case 'c': s_putc(&s, (char)va_arg(ap, int)); break;
            case 's': { const char* p = va_arg(ap, const char*); s_puts(&s, p ? p : "(null)"); } break;
            case 'd': {
                int32_t v = va_arg(ap, int32_t);
                if (v < 0) { s_putc(&s, '-'); s_putu(&s, (uint32_t)(-v), 10, 0, width, pad); }
                else       { s_putu(&s, (uint32_t)v, 10, 0, width, pad); }
            } break;
            case 'u': s_putu(&s, va_arg(ap, uint32_t), 10, 0, width, pad); break;
            case 'x': s_putu(&s, va_arg(ap, uint32_t), 16, 0, width, pad); break;
            case 'X': s_putu(&s, va_arg(ap, uint32_t), 16, 1, width, pad); break;
            case 'p': s_puts(&s, "0x"); s_putu(&s, (uint32_t)va_arg(ap, void*), 16, 1, 8, '0'); break;
            default:  s_putc(&s, '%'); s_putc(&s, *fmt); break;
        }
        if (!*fmt) break;
    }

    if (cap) buf[s.len < cap ? s.len : cap - 1] = 0;
    return (int)s.len;
}

int ksnprintf(char* buf, size_t cap, const char* fmt, ...){
    va_list ap; va_start(ap, fmt);
    int n = kvsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}
