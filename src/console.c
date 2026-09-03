// src/console.c — text console rendered onto the linear framebuffer (gfx.c).
// Same public API as the old VGA-text-mode version it replaces, so nothing
// upstream (kprintf, shell, exceptions.c) needed to change.
#include "console.h"
#include "header/gfx.h"
#include "header/font8x8.h"

#define SCALE  2
#define CELL_W ((FONT_GLYPH_W + 1) * SCALE)
#define CELL_H ((FONT_GLYPH_H + 1) * SCALE)
#define FG     GFX_RGB(0xC0, 0xC0, 0xC0)
#define BG     GFX_RGB(0x00, 0x00, 0x00)

static int row = 0, col = 0;
static int cols = 0, rows = 0;

void console_init(void){
    if (gfx_available()){
        cols = (int)(gfx_width()  / CELL_W);
        rows = (int)(gfx_height() / CELL_H);
        gfx_fill_rect(0, 0, gfx_width(), gfx_height(), BG);
    } else {
        cols = rows = 0;
    }
    row = col = 0;
}

static void newline(void){
    col = 0; row++;
    if (row >= rows) {
        gfx_scroll_up((uint32_t)CELL_H, BG);
        row = rows > 0 ? rows - 1 : 0;
    }
}

void console_putc(char ch){
    if (!gfx_available() || cols <= 0 || rows <= 0) return;

    if (ch == '\n') { newline(); return; }
    if (ch == '\r') { col = 0; return; }
    if (ch == '\b' || ch == 0x7F) {
        if (col > 0) {
            col--;
            gfx_draw_char((uint32_t)(col * CELL_W), (uint32_t)(row * CELL_H), ' ', FG, BG, SCALE);
        } else if (row > 0) {
            row--; col = cols - 1;
            gfx_draw_char((uint32_t)(col * CELL_W), (uint32_t)(row * CELL_H), ' ', FG, BG, SCALE);
        }
        return;
    }

    gfx_draw_char((uint32_t)(col * CELL_W), (uint32_t)(row * CELL_H), ch, FG, BG, SCALE);
    if (++col >= cols) newline();
}

void console_puts(const char* s){ while (*s) console_putc(*s++); }

void console_printu_at(int x, int y, unsigned long long n){
    if (!gfx_available()) return;
    char b[21]; int i = 0;
    if (!n) { gfx_draw_char((uint32_t)(x * CELL_W), (uint32_t)(y * CELL_H), '0', FG, BG, SCALE); return; }
    while (n) { b[i++] = '0' + (n % 10); n /= 10; }
    for (int k = 0; k < i; k++)
        gfx_draw_char((uint32_t)((x + k) * CELL_W), (uint32_t)(y * CELL_H), b[i - 1 - k], FG, BG, SCALE);
}
