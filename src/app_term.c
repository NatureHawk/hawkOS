// src/app_term.c — Terminal
//
// The interesting part of this app is how little of it there is. Instead of
// reimplementing the shell for the GUI, it installs a kprintf sink for the
// duration of one shell_exec_line() call, so every existing command writes
// into this window's character grid instead of onto the raw framebuffer
// console. Adding a shell command adds it to the terminal for free.
#include <stdint.h>
#include "header/apps.h"
#include "header/wm.h"
#include "header/gfx.h"
#include "header/kheap.h"
#include "header/kstring.h"
#include "header/kprintf.h"
#include "header/shell.h"
#include "header/kbd.h"
#include "header/font.h"
#include "header/clipboard.h"

extern volatile unsigned long long ticks;

#define TERM_COLS  78
#define TERM_ROWS  22
#define PAD        8
#define INPUT_MAX  120

#define COL_TERM_BG   GFX_RGB(0x12, 0x17, 0x1E)
#define COL_TERM_FG   GFX_RGB(0xD6, 0xDD, 0xE6)
#define COL_TERM_ECHO GFX_RGB(0x6F, 0xB5, 0xF0)
#define COL_TERM_CUR  GFX_RGB(0xF5, 0xC5, 0x42)
#define COL_TERM_SEL  GFX_RGB(0x2F, 0x5D, 0x9A)

#define DCLICK_TICKS  50

typedef struct {
    char grid[TERM_ROWS][TERM_COLS];
    int  cx, cy;
    char input[INPUT_MAX];
    int  ilen;

    // Text selection, as cell indices (row * TERM_COLS + col) into the grid.
    // sel_a..sel_b is the normalised, inclusive range, or -1 when nothing is
    // selected; `anchor` is where the press landed, which the range grows
    // away from in whichever direction the pointer travels.
    int  anchor, sel_a, sel_b;
    int  dragging, moved;
    unsigned long long click_when;
    int  click_count, click_cell;
} term_t;

// The sink is a plain function pointer with no user data, so the terminal
// being written to has to be reachable from a file-scope variable. Only one
// command runs at a time (shell_exec_line is synchronous on the WM task), so
// a single slot is enough — but it is set and cleared around exactly that
// call so nothing else can leak into a window.
static term_t* sink_target = 0;

static void term_scroll(term_t* t){
    for (int r = 0; r < TERM_ROWS - 1; r++)
        memcpy(t->grid[r], t->grid[r + 1], TERM_COLS);
    memset(t->grid[TERM_ROWS - 1], ' ', TERM_COLS);
    t->cy = TERM_ROWS - 1;
}

static void term_newline(term_t* t){
    t->cx = 0;
    if (++t->cy >= TERM_ROWS) term_scroll(t);
}

static void term_putc(term_t* t, char c){
    if (c == '\n'){ term_newline(t); return; }
    if (c == '\r'){ t->cx = 0; return; }
    if (c == '\b'){ if (t->cx > 0) t->grid[t->cy][--t->cx] = ' '; return; }
    if (c == '\t'){ for (int i = 0; i < 4; i++) term_putc(t, ' '); return; }
    if (c < 32 || c > 126) return;

    t->grid[t->cy][t->cx] = c;
    if (++t->cx >= TERM_COLS) term_newline(t);
}

static void term_puts(term_t* t, const char* s){ while (*s) term_putc(t, *s++); }

static void sink(char c){ if (sink_target) term_putc(sink_target, c); }

static void term_clear(term_t* t){
    memset(t->grid, ' ', sizeof(t->grid));
    t->cx = t->cy = 0;
}

static void run_line(term_t* t){
    t->input[t->ilen] = 0;

    term_puts(t, "> ");
    term_puts(t, t->input);
    term_newline(t);

    // Two commands cannot be delegated: `clear` wipes the whole framebuffer
    // console, and `gui` would re-enter the desktop from inside itself.
    if (strcmp(t->input, "clear") == 0){
        term_clear(t);
    } else if (strcmp(t->input, "gui") == 0){
        term_puts(t, "already in the desktop"); term_newline(t);
    } else if (t->ilen > 0){
        sink_target = t;
        kprintf_set_sink(sink);
        shell_exec_line(t->input);
        kprintf_set_sink(0);
        sink_target = 0;
    }

    t->ilen = 0;
    t->input[0] = 0;
}

// ------------------------------------------------------------- selection

static int cell_at(int px, int py){
    int c = (px - PAD) / FONT_W, r = (py - PAD) / FONT_H;
    if (px < PAD) c = 0;
    if (py < PAD) r = 0;
    if (c < 0) c = 0;
    if (c >= TERM_COLS) c = TERM_COLS - 1;
    if (r < 0) r = 0;
    if (r >= TERM_ROWS) r = TERM_ROWS - 1;
    return r * TERM_COLS + c;
}

static void select_range(term_t* t, int a, int b){
    if (a > b){ int k = a; a = b; b = k; }
    t->sel_a = a;
    t->sel_b = b;
}

static void select_word(term_t* t, int cell){
    int r = cell / TERM_COLS, c = cell % TERM_COLS;
    if (t->grid[r][c] == ' '){ select_range(t, cell, cell); return; }
    int a = c, b = c;
    while (a > 0 && t->grid[r][a - 1] != ' ') a--;
    while (b < TERM_COLS - 1 && t->grid[r][b + 1] != ' ') b++;
    select_range(t, r * TERM_COLS + a, r * TERM_COLS + b);
}

static void copy_selection(term_t* t){
    if (t->sel_a < 0) return;
    char out[TERM_ROWS * (TERM_COLS + 1) + 1];
    int n = 0;
    int r0 = t->sel_a / TERM_COLS, r1 = t->sel_b / TERM_COLS;
    for (int r = r0; r <= r1; r++){
        int c0 = (r == r0) ? t->sel_a % TERM_COLS : 0;
        int c1 = (r == r1) ? t->sel_b % TERM_COLS : TERM_COLS - 1;
        // Trailing blanks are grid padding, not text somebody wrote.
        while (c1 >= c0 && t->grid[r][c1] == ' ') c1--;
        for (int c = c0; c <= c1; c++) out[n++] = t->grid[r][c] ? t->grid[r][c] : ' ';
        if (r < r1) out[n++] = '\n';
    }
    clip_set_text(out, (uint32_t)n);
}

// One line of the clipboard goes onto the input line. Never more: a paste that
// carried a newline and ran what followed would be a way for text copied from
// a web page to execute itself.
static void paste_input(term_t* t){
    uint32_t n;
    const char* s = clip_text(&n);
    for (uint32_t i = 0; i < n; i++){
        if (s[i] == '\n' || s[i] == '\r') break;
        if (s[i] >= 32 && s[i] <= 126 && t->ilen < INPUT_MAX - 1) t->input[t->ilen++] = s[i];
    }
}

static void paint(wm_window_t* win, term_t* t){
    int x, y, w, h;
    wm_client_rect(win, &x, &y, &w, &h);

    gfx_fill_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, COL_TERM_BG);

    if (t->sel_a >= 0){
        for (int r = 0; r < TERM_ROWS; r++){
            int ry = y + PAD + r * FONT_H;
            if (ry + FONT_H > y + h - FONT_H - PAD) break;
            int lo = r * TERM_COLS, hi = lo + TERM_COLS - 1;
            int a = t->sel_a > lo ? t->sel_a : lo;
            int b = t->sel_b < hi ? t->sel_b : hi;
            if (a > b) continue;
            gfx_fill_rect((uint32_t)(x + PAD + (a - lo) * FONT_W), (uint32_t)ry,
                          (uint32_t)((b - a + 1) * FONT_W), FONT_H, COL_TERM_SEL);
        }
    }

    for (int r = 0; r < TERM_ROWS; r++){
        int ry = y + PAD + r * FONT_H;
        if (ry + FONT_H > y + h - FONT_H - PAD) break;
        for (int c = 0; c < TERM_COLS; c++){
            char ch = t->grid[r][c];
            if (ch == ' ' || ch == 0) continue;
            gfx_char16((uint32_t)(x + PAD + c * FONT_W), (uint32_t)ry, ch,
                       COL_TERM_FG, GFX_TRANSPARENT);
        }
    }

    // The prompt is drawn separately, pinned to the bottom, so a long
    // command never scrolls out from under the cursor while it is typed.
    int py = y + h - FONT_H - PAD;
    gfx_text((uint32_t)(x + PAD), (uint32_t)py, "> ", COL_TERM_ECHO, GFX_TRANSPARENT);
    gfx_text_n((uint32_t)(x + PAD + 2 * FONT_W), (uint32_t)py, t->input,
               (uint32_t)t->ilen, COL_TERM_FG, GFX_TRANSPARENT);

    if (wm_is_focused(win))
        gfx_fill_rect((uint32_t)(x + PAD + (2 + t->ilen) * FONT_W), (uint32_t)py,
                      FONT_W, FONT_H, COL_TERM_CUR);
}

static void handler(wm_window_t* win, const wm_event_t* ev){
    term_t* t = (term_t*)win->user;
    if (!t) return;

    switch (ev->type){
        case WM_EV_PAINT:
            paint(win, t);
            break;

        case WM_EV_MOUSE_DOWN: {
            int cell = cell_at(ev->x, ev->y);
            int dbl = (ticks - t->click_when < DCLICK_TICKS && cell == t->click_cell);
            t->click_count = dbl ? t->click_count + 1 : 1;
            t->click_when = ticks;
            t->click_cell = cell;
            t->anchor = cell;
            t->moved = 0;
            t->sel_a = t->sel_b = -1;
            if (t->click_count == 2){
                select_word(t, cell);
            } else if (t->click_count >= 3){
                int r = cell / TERM_COLS;
                select_range(t, r * TERM_COLS, r * TERM_COLS + TERM_COLS - 1);
                t->click_count = 0;
            }
            t->dragging = 1;
            wm_invalidate();
        } break;

        case WM_EV_MOUSE_MOVE:
            if (t->dragging){
                int cell = cell_at(ev->x, ev->y);
                if (cell != t->anchor || t->moved){
                    t->moved = 1;
                    select_range(t, t->anchor, cell);
                    wm_invalidate();
                }
            }
            break;

        case WM_EV_MOUSE_UP:
            t->dragging = 0;
            break;

        case WM_EV_KEY:
            if (kbd_ctrl_down()){
                int k = (ev->key >= 'A' && ev->key <= 'Z') ? ev->key + 32 : ev->key;
                if (k == 'c')      copy_selection(t);
                else if (k == 'v') { t->sel_a = t->sel_b = -1; paste_input(t); }
                else if (k == 'a') select_range(t, 0, TERM_ROWS * TERM_COLS - 1);
                wm_invalidate();
                break;
            }
            // Typing dismisses the selection, the way it does everywhere.
            t->sel_a = t->sel_b = -1;
            if (ev->key == '\n')                       run_line(t);
            else if (ev->key == '\b'){ if (t->ilen) t->ilen--; }
            else if (ev->key >= 32 && ev->key <= 126){
                if (t->ilen < INPUT_MAX - 1) t->input[t->ilen++] = (char)ev->key;
            }
            wm_invalidate();
            break;

        case WM_EV_CLOSE:
            if (sink_target == t){ kprintf_set_sink(0); sink_target = 0; }
            kfree(t);
            win->user = 0;
            break;

        default: break;
    }
}

void app_term_open(void){
    term_t* t = (term_t*)kmalloc(sizeof(term_t));
    if (!t) return;
    memset(t, 0, sizeof(*t));
    t->sel_a = t->sel_b = -1;
    term_clear(t);

    shell_init();
    term_puts(t, "hawkOS terminal - type 'help'");
    term_newline(t);

    int w = TERM_COLS * FONT_W + 2 * PAD + 2;
    int h = (TERM_ROWS + 2) * FONT_H + 2 * PAD + WM_TITLE_H;
    wm_open("Terminal", 90, 70, w, h, handler, t);
}
