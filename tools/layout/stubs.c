// tools/layout/stubs.c — host-side stand-ins for the kernel services that
// src/html.c and src/css.c call into.
//
// The point of this harness is to run the real layout pass over a real page
// on a workstation, in milliseconds, instead of booting QEMU for two minutes
// to look at a screenshot. Everything here is therefore either a one-line
// forward to libc (the allocator) or the exact measurement code from gfx.c
// (the font metrics) -- nothing is approximated, because a harness that
// measures text differently from the kernel would report line breaks that
// never happen on the real thing.
#include <stdint.h>
#include <stdlib.h>
#include "header/gfx.h"
#include "header/fontprop.h"

// ------------------------------------------------------------- allocator

void* kmalloc(uint32_t n){ return malloc(n ? n : 1); }
void  kfree(void* p){ free(p); }

// -------------------------------------------------------- font metrics

// Same lookup as gfx.c: characters outside the table fall back to the space
// glyph, so an out-of-range byte still advances the pen by something sane.
static const pf_glyph_t* pf_lookup(const pf_face_t* f, char c){
    unsigned char u = (unsigned char)c;
    if (u < PF_FIRST || u > PF_LAST) u = '?';
    return &f->glyphs[u - PF_FIRST];
}

uint32_t gfx_pf_width_n(const char* s, uint32_t n, const pf_face_t* f){
    uint32_t pen = 0;
    for (uint32_t i = 0; i < n && s[i]; i++) pen += pf_lookup(f, s[i])->adv;
    return pen;
}

uint32_t gfx_pf_width(const char* s, const pf_face_t* f){
    uint32_t pen = 0;
    while (*s) pen += pf_lookup(f, *s++)->adv;
    return pen;
}

// ------------------------------------------------------------- painting

// The harness never rasterises, so every drawing entry point is a no-op that
// still returns the advance its caller expects.
uint32_t gfx_pf_char(uint32_t x, uint32_t y, char c, const pf_face_t* f, uint32_t fg){
    (void)x; (void)y; (void)fg;
    return pf_lookup(f, c)->adv;
}

uint32_t gfx_pf_text_n(uint32_t x, uint32_t y, const char* s, uint32_t n,
                       const pf_face_t* f, uint32_t fg){
    (void)x; (void)y; (void)fg;
    return gfx_pf_width_n(s, n, f);
}

void gfx_text_n(uint32_t x, uint32_t y, const char* s, uint32_t n, uint32_t fg, uint32_t bg){
    (void)x; (void)y; (void)s; (void)n; (void)fg; (void)bg;
}
void gfx_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t c){
    (void)x; (void)y; (void)w; (void)h; (void)c;
}
void gfx_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t c){
    (void)x; (void)y; (void)w; (void)h; (void)c;
}
void gfx_fill_circle(int cx, int cy, int r, uint32_t c){ (void)cx; (void)cy; (void)r; (void)c; }
void gfx_hline(uint32_t x, uint32_t y, uint32_t w, uint32_t c){ (void)x; (void)y; (void)w; (void)c; }
void gfx_vline(uint32_t x, uint32_t y, uint32_t h, uint32_t c){ (void)x; (void)y; (void)h; (void)c; }
