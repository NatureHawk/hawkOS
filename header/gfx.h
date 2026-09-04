#pragma once
#include <stdint.h>
#include "header/fontprop.h"

#define GFX_RGB(r,g,b) (((uint32_t)(r)<<16)|((uint32_t)(g)<<8)|(uint32_t)(b))
#define GFX_TRANSPARENT 0xFFFFFFFFu   // sentinel bg: skip unset pixels instead of painting them

// Largest mode the software back buffer can cover. GRUB picks the mode, not
// us, so anything larger simply runs without double buffering rather than
// scribbling past the end of the buffer.
#define GFX_MAX_W 1280u
#define GFX_MAX_H 800u

// Reads the framebuffer handed back by GRUB (requested in boot.s's multiboot
// header) and identity-maps it. Must run after paging_init(). Returns 0 if a
// usable 32bpp linear RGB framebuffer was granted, -1 otherwise (caller
// should fall back to text-only / serial output).
int gfx_init(uint32_t mb_magic, uint32_t mb_info_addr);

int      gfx_available(void);
uint32_t gfx_width(void);
uint32_t gfx_height(void);

// Double buffering. The text console draws straight to the visible
// framebuffer (one character at a time, so tearing is a non-issue), while
// the window manager composites a whole frame off-screen and presents it in
// one pass. gfx_use_backbuffer() picks which surface the drawing calls
// below write to; it silently stays on the front buffer if the mode is too
// large for the static back buffer.
void gfx_use_backbuffer(int on);
int  gfx_has_backbuffer(void);
void gfx_present(void);                                       // whole back buffer -> screen
void gfx_present_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

// Clip rectangle, in screen coordinates. Every drawing call below is
// intersected with it, which is what lets a window paint its contents
// without having to clip each primitive itself.
void gfx_clip_set(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void gfx_clip_reset(void);

void     gfx_put_pixel(uint32_t x, uint32_t y, uint32_t color);
uint32_t gfx_get_pixel(uint32_t x, uint32_t y);
void     gfx_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void     gfx_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void     gfx_hline(uint32_t x, uint32_t y, uint32_t w, uint32_t color);
void     gfx_fill_circle(int cx, int cy, int r, uint32_t color);
void     gfx_draw_circle(int cx, int cy, int r, uint32_t color);
// Vertical two-colour gradient, one solid row per scanline. Used for the
// desktop wallpaper, where a flat fill looks unfinished.
void     gfx_vgradient(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t top, uint32_t bot);
void     gfx_vline(uint32_t x, uint32_t y, uint32_t h, uint32_t color);

// 5x7 font (see font8x8.h) — used by the text console.
void gfx_draw_char(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg, int scale);
void gfx_draw_string(uint32_t x, uint32_t y, const char* s, uint32_t fg, uint32_t bg, int scale);


// 8x16 font (see font.h) - used by the window manager, the desktop and the
// browser. Pass GFX_TRANSPARENT as bg to leave the background untouched.
void     gfx_text(uint32_t x, uint32_t y, const char* s, uint32_t fg, uint32_t bg);
void     gfx_text_n(uint32_t x, uint32_t y, const char* s, uint32_t n, uint32_t fg, uint32_t bg);
void     gfx_char16(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg);
uint32_t gfx_text_width(const char* s);
void     gfx_char16_scaled(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg, int scale);
void     gfx_text_scaled(uint32_t x, uint32_t y, const char* s, uint32_t fg, uint32_t bg, int scale);

// Proportional anti-aliased text (see header/fontprop.h). `y` is the top of
// the line box, not the baseline, so callers can lay out lines without
// knowing a face's metrics. There is no bg parameter: coverage glyphs blend
// with whatever is already on the surface, which is the point of them. All
// three return the pen advance in pixels, so measuring and drawing share one
// code path and can never disagree about how wide a string is.
uint32_t gfx_pf_char(uint32_t x, uint32_t y, char c, const pf_face_t* f, uint32_t fg);
uint32_t gfx_pf_text_n(uint32_t x, uint32_t y, const char* s, uint32_t n,
                       const pf_face_t* f, uint32_t fg);
uint32_t gfx_pf_width_n(const char* s, uint32_t n, const pf_face_t* f);
uint32_t gfx_pf_width(const char* s, const pf_face_t* f);

// Shifts the whole framebuffer up by rows_px rows, filling the exposed
// bottom strip with bg. Used by the text console to scroll.
void gfx_scroll_up(uint32_t rows_px, uint32_t bg);
