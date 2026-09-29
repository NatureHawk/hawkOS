// src/lineedit.c — single-line text field
#include <stdint.h>
#include "header/lineedit.h"
#include "header/clipboard.h"
#include "header/gfx.h"
#include "header/theme.h"
#include "header/kbd.h"
#include "header/kstring.h"
#include "header/font.h"

static void sel_range(const lineedit_t* le, int* a, int* b){
    if (le->cur < le->anc){ *a = le->cur; *b = le->anc; }
    else                  { *a = le->anc; *b = le->cur; }
}

int le_has_selection(const lineedit_t* le){ return le->cur != le->anc; }

void le_set(lineedit_t* le, const char* s, int select_all){
    int n = s ? (int)strlen(s) : 0;
    if (n > LE_MAX - 1) n = LE_MAX - 1;
    if (n) memcpy(le->text, s, (size_t)n);
    le->text[n] = 0;
    le->len = n;
    le->cur = n;
    le->anc = select_all ? 0 : n;
    le->scroll = 0;
}

static void delete_selection(lineedit_t* le){
    int a, b;
    sel_range(le, &a, &b);
    if (a == b) return;
    memmove(le->text + a, le->text + b, (size_t)(le->len - b + 1));
    le->len -= b - a;
    le->cur = le->anc = a;
}

static void insert_text(lineedit_t* le, const char* s, int n){
    delete_selection(le);
    if (n > LE_MAX - 1 - le->len) n = LE_MAX - 1 - le->len;
    if (n <= 0) return;
    memmove(le->text + le->cur + n, le->text + le->cur, (size_t)(le->len - le->cur + 1));
    memcpy(le->text + le->cur, s, (size_t)n);
    le->len += n;
    le->cur += n;
    le->anc = le->cur;
}

static void copy_selection(const lineedit_t* le){
    int a, b;
    sel_range(le, &a, &b);
    if (a != b) clip_set_text(le->text + a, (uint32_t)(b - a));
}

// A field holds one line, so pasting a block of text takes its first line and
// drops anything unprintable rather than smuggling a newline into a file name.
static void paste(lineedit_t* le){
    uint32_t n;
    const char* t = clip_text(&n);
    char tmp[LE_MAX];
    int k = 0;
    for (uint32_t i = 0; i < n && k < LE_MAX - 1; i++){
        if (t[i] == '\n' || t[i] == '\r') break;
        if (t[i] >= 32 && t[i] <= 126) tmp[k++] = t[i];
    }
    if (k) insert_text(le, tmp, k);
}

static void move_to(lineedit_t* le, int pos, int extend){
    if (pos < 0) pos = 0;
    if (pos > le->len) pos = le->len;
    le->cur = pos;
    if (!extend) le->anc = pos;
}

int le_key(lineedit_t* le, int key){
    int shift = kbd_shift_down();

    if (kbd_ctrl_down()){
        int k = (key >= 'A' && key <= 'Z') ? key + 32 : key;
        switch (k){
            case 'a': le->anc = 0; le->cur = le->len; return 1;
            case 'c': copy_selection(le); return 1;
            case 'x': copy_selection(le); delete_selection(le); return 1;
            case 'v': paste(le); return 1;
            default:  return 0;
        }
    }

    switch (key){
        case KEY_LEFT:
            if (le_has_selection(le) && !shift){ int a, b; sel_range(le, &a, &b); move_to(le, a, 0); }
            else move_to(le, le->cur - 1, shift);
            return 1;
        case KEY_RIGHT:
            if (le_has_selection(le) && !shift){ int a, b; sel_range(le, &a, &b); move_to(le, b, 0); }
            else move_to(le, le->cur + 1, shift);
            return 1;
        case KEY_HOME: move_to(le, 0, shift);       return 1;
        case KEY_END:  move_to(le, le->len, shift); return 1;
        case '\b':
            if (le_has_selection(le)) delete_selection(le);
            else if (le->cur > 0){
                memmove(le->text + le->cur - 1, le->text + le->cur, (size_t)(le->len - le->cur + 1));
                le->len--; le->cur--; le->anc = le->cur;
            }
            return 1;
        case KEY_DELETE:
            if (le_has_selection(le)) delete_selection(le);
            else if (le->cur < le->len){
                memmove(le->text + le->cur, le->text + le->cur + 1, (size_t)(le->len - le->cur));
                le->len--;
            }
            return 1;
        default:
            if (key >= 32 && key <= 126){ char c = (char)key; insert_text(le, &c, 1); return 1; }
            return 0;
    }
}

static int col_at(const lineedit_t* le, int px){
    int col = (px + FONT_W / 2) / FONT_W + le->scroll;
    if (px < 0) col = le->scroll - 1;
    return col;
}

void le_click(lineedit_t* le, int px, int extend){
    move_to(le, col_at(le, px), extend);
}

void le_drag(lineedit_t* le, int px){
    move_to(le, col_at(le, px), 1);
}

void le_draw(lineedit_t* le, int x, int y, int w, int h, int focused){
    int cols = w / FONT_W;
    if (cols < 1) cols = 1;

    // Keep the caret on screen, with a column of margin so it is not flush
    // against the edge of the box.
    if (le->cur < le->scroll) le->scroll = le->cur;
    if (le->cur > le->scroll + cols - 2) le->scroll = le->cur - (cols - 2);
    if (le->scroll < 0) le->scroll = 0;

    int ty = y + (h - FONT_H) / 2;
    int a, b;
    sel_range(le, &a, &b);

    gfx_clip_set((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h);
    if (a != b){
        int sa = a - le->scroll, sb = b - le->scroll;
        if (sa < 0) sa = 0;
        if (sb > cols) sb = cols;
        if (sb > sa)
            gfx_fill_rect((uint32_t)(x + sa * FONT_W), (uint32_t)ty,
                          (uint32_t)((sb - sa) * FONT_W), FONT_H, TH_ACCENT_DIM);
    }
    if (le->scroll < le->len)
        gfx_text_n((uint32_t)x, (uint32_t)ty, le->text + le->scroll,
                   (uint32_t)(le->len - le->scroll), TH_TEXT, GFX_TRANSPARENT);
    if (focused)
        gfx_fill_rect((uint32_t)(x + (le->cur - le->scroll) * FONT_W), (uint32_t)ty, 2, FONT_H,
                      TH_ACCENT);
    gfx_clip_reset();
}
