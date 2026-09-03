// src/gfx.c — linear framebuffer primitives, backed by whatever GRUB granted
// from the video-mode request in boot.s's multiboot header.
//
// Two surfaces share these primitives. The text console writes a character
// at a time and wants it on screen immediately, so it draws to the visible
// framebuffer. The window manager repaints the whole scene every frame, and
// doing that on the visible surface would show every intermediate state as
// flicker — so it draws to a static back buffer and blits once. The active
// surface is a pair of (base, pitch) values rather than a branch in every
// primitive, which keeps the inner loops tight.
#include <stdint.h>
#include "header/gfx.h"
#include "header/multiboot.h"
#include "header/paging.h"
#include "header/font8x8.h"
#include "header/font.h"
#include "header/kprintf.h"

static uint8_t* fb        = 0;
static uint32_t fb_pitch  = 0;
static uint32_t fb_w      = 0;
static uint32_t fb_h      = 0;
static int      available = 0;

// 4 MB of .bss. It lives inside [kernel_start, kernel_end), so pmm_init()
// already reserves it and paging_init()'s identity map already covers it.
static uint32_t backbuf[GFX_MAX_W * GFX_MAX_H];
static int      back_ok  = 0;    // mode fits in backbuf
static int      back_on  = 0;    // drawing calls currently target it

static uint8_t* dst_base  = 0;
static uint32_t dst_pitch = 0;

static uint32_t clip_x0 = 0, clip_y0 = 0, clip_x1 = 0, clip_y1 = 0;

static void retarget(void){
    if (back_on && back_ok) {
        dst_base  = (uint8_t*)backbuf;
        dst_pitch = fb_w * 4u;
    } else {
        dst_base  = fb;
        dst_pitch = fb_pitch;
    }
}

int gfx_init(uint32_t mb_magic, uint32_t mb_info_addr){
    if (mb_magic != MULTIBOOT_BOOTLOADER_MAGIC || mb_info_addr == 0) {
        kprintf("[gfx] no multiboot info; graphics unavailable\n");
        return -1;
    }
    const multiboot_info_t* mbi = (const multiboot_info_t*)mb_info_addr;

    if (!(mbi->flags & MULTIBOOT_FLAG_FB)) {
        kprintf("[gfx] bootloader did not report a framebuffer\n");
        return -1;
    }
    if (mbi->framebuffer_bpp != 32 || mbi->framebuffer_type != MULTIBOOT_FB_TYPE_RGB) {
        kprintf("[gfx] framebuffer is %u bpp / type %u, v1 only supports 32bpp RGB\n",
                mbi->framebuffer_bpp, mbi->framebuffer_type);
        return -1;
    }

    uint32_t phys = (uint32_t)mbi->framebuffer_addr;
    fb_pitch = mbi->framebuffer_pitch;
    fb_w     = mbi->framebuffer_width;
    fb_h     = mbi->framebuffer_height;

    uint32_t region_start = phys & 0xFFFFF000u;
    uint32_t region_end   = (phys + fb_pitch * fb_h + 0xFFFu) & 0xFFFFF000u;
    paging_identity_map_range(region_start, region_end, PAGE_RW);

    fb = (uint8_t*)phys;
    available = 1;

    back_ok = (fb_w <= GFX_MAX_W && fb_h <= GFX_MAX_H);
    back_on = 0;
    retarget();
    gfx_clip_reset();

    kprintf("[gfx] framebuffer %ux%u @32bpp, phys=%p, pitch=%u\n", fb_w, fb_h, (void*)phys, fb_pitch);
    kprintf("[gfx] back buffer %s (%u KB)\n",
            back_ok ? "ready" : "disabled: mode larger than GFX_MAX",
            (unsigned)(sizeof(backbuf) / 1024u));
    return 0;
}

int      gfx_available(void){ return available; }
uint32_t gfx_width(void){ return fb_w; }
uint32_t gfx_height(void){ return fb_h; }
int      gfx_has_backbuffer(void){ return back_ok; }

void gfx_use_backbuffer(int on){
    back_on = on ? 1 : 0;
    retarget();
}

void gfx_clip_reset(void){
    clip_x0 = 0; clip_y0 = 0; clip_x1 = fb_w; clip_y1 = fb_h;
}

void gfx_clip_set(uint32_t x, uint32_t y, uint32_t w, uint32_t h){
    uint32_t x1 = x + w, y1 = y + h;
    if (x1 > fb_w) x1 = fb_w;
    if (y1 > fb_h) y1 = fb_h;
    if (x  > fb_w) x  = fb_w;
    if (y  > fb_h) y  = fb_h;
    clip_x0 = x; clip_y0 = y;
    clip_x1 = x1 > x ? x1 : x;
    clip_y1 = y1 > y ? y1 : y;
}

void gfx_present(void){
    if (!available || !back_ok) return;
    gfx_present_rect(0, 0, fb_w, fb_h);
}

void gfx_present_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h){
    if (!available || !back_ok) return;
    uint32_t x1 = x + w, y1 = y + h;
    if (x1 > fb_w) x1 = fb_w;
    if (y1 > fb_h) y1 = fb_h;
    if (x >= x1 || y >= y1) return;

    uint32_t span = x1 - x;
    for (uint32_t yy = y; yy < y1; yy++) {
        const uint32_t* s = &backbuf[yy * fb_w + x];
        uint32_t*       d = (uint32_t*)(fb + yy * fb_pitch + x * 4u);
        for (uint32_t i = 0; i < span; i++) d[i] = s[i];
    }
}

void gfx_put_pixel(uint32_t x, uint32_t y, uint32_t color){
    if (!available) return;
    if (x < clip_x0 || x >= clip_x1 || y < clip_y0 || y >= clip_y1) return;
    *(uint32_t*)(dst_base + y * dst_pitch + x * 4u) = color;
}

uint32_t gfx_get_pixel(uint32_t x, uint32_t y){
    if (!available || x >= fb_w || y >= fb_h) return 0;
    return *(uint32_t*)(dst_base + y * dst_pitch + x * 4u);
}

// Clipped once, then filled with straight word stores. The old version
// called gfx_put_pixel per pixel, which meant four bounds checks and a
// multiply for every one of the ~1M pixels in a full-screen repaint.
void gfx_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color){
    if (!available) return;
    uint32_t x0 = x < clip_x0 ? clip_x0 : x;
    uint32_t y0 = y < clip_y0 ? clip_y0 : y;
    uint32_t x1 = x + w, y1 = y + h;
    if (x1 > clip_x1) x1 = clip_x1;
    if (y1 > clip_y1) y1 = clip_y1;
    if (x0 >= x1 || y0 >= y1) return;

    uint32_t span = x1 - x0;
    for (uint32_t yy = y0; yy < y1; yy++) {
        uint32_t* d = (uint32_t*)(dst_base + yy * dst_pitch + x0 * 4u);
        for (uint32_t i = 0; i < span; i++) d[i] = color;
    }
}

void gfx_hline(uint32_t x, uint32_t y, uint32_t w, uint32_t color){ gfx_fill_rect(x, y, w, 1, color); }
void gfx_vline(uint32_t x, uint32_t y, uint32_t h, uint32_t color){ gfx_fill_rect(x, y, 1, h, color); }

void gfx_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color){
    if (w == 0 || h == 0) return;
    gfx_hline(x, y,         w, color);
    gfx_hline(x, y + h - 1, w, color);
    gfx_vline(x,         y, h, color);
    gfx_vline(x + w - 1, y, h, color);
}

void gfx_draw_char(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg, int scale){
    if (!available) return;
    if (scale < 1) scale = 1;
    uint8_t rows[FONT_GLYPH_H];
    font_glyph(c, rows);
    for (int ry = 0; ry < FONT_GLYPH_H; ry++){
        uint8_t row = rows[ry];
        for (int cx = 0; cx < FONT_GLYPH_W; cx++){
            int bit = (row >> (FONT_GLYPH_W - 1 - cx)) & 1;
            if (!bit && bg == GFX_TRANSPARENT) continue;
            gfx_fill_rect(x + (uint32_t)cx * (uint32_t)scale,
                          y + (uint32_t)ry * (uint32_t)scale,
                          (uint32_t)scale, (uint32_t)scale,
                          bit ? fg : bg);
        }
    }
}

void gfx_draw_string(uint32_t x, uint32_t y, const char* s, uint32_t fg, uint32_t bg, int scale){
    if (scale < 1) scale = 1;
    uint32_t cx = x;
    while (*s){
        gfx_draw_char(cx, y, *s, fg, bg, scale);
        cx += (uint32_t)(FONT_GLYPH_W + 1) * (uint32_t)scale;
        s++;
    }
}

void gfx_scroll_up(uint32_t rows_px, uint32_t bg){
    if (!available) return;
    if (rows_px >= fb_h) { gfx_fill_rect(0, 0, fb_w, fb_h, bg); return; }
    for (uint32_t y = 0; y < fb_h - rows_px; y++){
        uint8_t* dst = dst_base + y * dst_pitch;
        uint8_t* src = dst_base + (y + rows_px) * dst_pitch;
        for (uint32_t b = 0; b < fb_w * 4u; b++) dst[b] = src[b];
    }
    gfx_fill_rect(0, fb_h - rows_px, fb_w, rows_px, bg);
}

// ---------------------------------------------------------------- 8x16 text

void gfx_char16(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg){
    if (!available) return;
    const uint8_t* g = font8x16[(unsigned char)c];

    // Reject the glyph box against the clip rect once, rather than testing
    // each of the 128 pixels individually.
    if (x + FONT_W <= clip_x0 || x >= clip_x1 || y + FONT_H <= clip_y0 || y >= clip_y1) return;

    for (uint32_t ry = 0; ry < FONT_H; ry++){
        uint32_t py = y + ry;
        if (py < clip_y0 || py >= clip_y1) continue;
        uint8_t   row = g[ry];
        uint32_t* d   = (uint32_t*)(dst_base + py * dst_pitch);
        for (uint32_t cx = 0; cx < FONT_W; cx++){
            uint32_t px = x + cx;
            if (px < clip_x0 || px >= clip_x1) continue;
            if (row & (0x80u >> cx))      d[px] = fg;
            else if (bg != GFX_TRANSPARENT) d[px] = bg;
        }
    }
}

void gfx_text_n(uint32_t x, uint32_t y, const char* s, uint32_t n, uint32_t fg, uint32_t bg){
    for (uint32_t i = 0; i < n && s[i]; i++)
        gfx_char16(x + i * FONT_W, y, s[i], fg, bg);
}

void gfx_text(uint32_t x, uint32_t y, const char* s, uint32_t fg, uint32_t bg){
    uint32_t cx = x;
    while (*s){ gfx_char16(cx, y, *s++, fg, bg); cx += FONT_W; }
}

uint32_t gfx_text_width(const char* s){
    uint32_t n = 0;
    while (s[n]) n++;
    return n * FONT_W;
}

// ------------------------------------------------------- circles, gradients

void gfx_fill_circle(int cx, int cy, int r, uint32_t color){
    if (r <= 0) return;
    for (int dy = -r; dy <= r; dy++){
        // Half-width of the circle at this scanline, so each row is one
        // clipped fill instead of a per-pixel distance test.
        int span = 0;
        while ((span + 1) * (span + 1) + dy * dy <= r * r) span++;
        if (span > 0)
            gfx_fill_rect((uint32_t)(cx - span), (uint32_t)(cy + dy),
                          (uint32_t)(2 * span), 1, color);
    }
}

void gfx_draw_circle(int cx, int cy, int r, uint32_t color){
    if (r <= 0) return;
    for (int dy = -r; dy <= r; dy++){
        int span = 0;
        while ((span + 1) * (span + 1) + dy * dy <= r * r) span++;
        gfx_put_pixel((uint32_t)(cx - span), (uint32_t)(cy + dy), color);
        gfx_put_pixel((uint32_t)(cx + span), (uint32_t)(cy + dy), color);
    }
    for (int dx = -r; dx <= r; dx++){
        int span = 0;
        while ((span + 1) * (span + 1) + dx * dx <= r * r) span++;
        gfx_put_pixel((uint32_t)(cx + dx), (uint32_t)(cy - span), color);
        gfx_put_pixel((uint32_t)(cx + dx), (uint32_t)(cy + span), color);
    }
}

void gfx_vgradient(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t top, uint32_t bot){
    if (h == 0) return;
    int r0 = (int)((top >> 16) & 0xFF), g0 = (int)((top >> 8) & 0xFF), b0 = (int)(top & 0xFF);
    int r1 = (int)((bot >> 16) & 0xFF), g1 = (int)((bot >> 8) & 0xFF), b1 = (int)(bot & 0xFF);
    for (uint32_t i = 0; i < h; i++){
        int r = r0 + (r1 - r0) * (int)i / (int)h;
        int g = g0 + (g1 - g0) * (int)i / (int)h;
        int b = b0 + (b1 - b0) * (int)i / (int)h;
        gfx_fill_rect(x, y + i, w, 1, GFX_RGB(r, g, b));
    }
}

// Scaled glyph drawing: each font pixel becomes a scale x scale block. One
// bitmap font at two sizes is enough to give headings visual weight in the
// browser without carrying a second font in the kernel image.
void gfx_char16_scaled(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg, int scale){
    if (!available) return;
    if (scale <= 1){ gfx_char16(x, y, c, fg, bg); return; }

    const uint8_t* g = font8x16[(unsigned char)c];
    for (uint32_t ry = 0; ry < FONT_H; ry++){
        uint8_t row = g[ry];
        for (uint32_t cx = 0; cx < FONT_W; cx++){
            int on = (row & (0x80u >> cx)) != 0;
            if (!on && bg == GFX_TRANSPARENT) continue;
            gfx_fill_rect(x + cx * (uint32_t)scale, y + ry * (uint32_t)scale,
                          (uint32_t)scale, (uint32_t)scale, on ? fg : bg);
        }
    }
}

void gfx_text_scaled(uint32_t x, uint32_t y, const char* s, uint32_t fg, uint32_t bg, int scale){
    if (scale < 1) scale = 1;
    uint32_t cx = x;
    while (*s){ gfx_char16_scaled(cx, y, *s++, fg, bg, scale); cx += (uint32_t)(FONT_W * scale); }
}
