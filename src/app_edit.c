// src/app_edit.c — Editor
//
// A plain-text editor: one flat byte buffer, a caret, an optional selection,
// and an index of where each line starts. Everything the rest of the system
// hands it -- a file opened from Files, text from the clipboard -- is bytes in
// that buffer, and everything it hands back is the buffer written to disk.
//
// The line index is rebuilt after every edit. That is a pass over the buffer,
// which for the 128 KB this editor accepts costs less than painting one
// frame, and it removes the class of bug where an incremental index and the
// text disagree about where a line begins.
//
// Undo is a stack of whole-buffer snapshots rather than a log of edits. It
// spends memory to save code that has to be right about inverting every kind
// of change; runs of typing are merged into one step so a snapshot is taken
// per burst of keystrokes, not per character.
#include <stdint.h>
#include "header/apps.h"
#include "header/wm.h"
#include "header/theme.h"
#include "header/gfx.h"
#include "header/kheap.h"
#include "header/kstring.h"
#include "header/kbd.h"
#include "header/mouse.h"
#include "header/font.h"
#include "header/fat32.h"
#include "header/fsutil.h"
#include "header/clipboard.h"
#include "header/lineedit.h"
#include "header/shell.h"

extern volatile unsigned long long ticks;

#define EDIT_MAX     (128u * 1024u)
#define TOOLBAR_H    38
#define BAR_H        42
#define STATUS_H     24
#define SCROLL_W     12
#define UNDO_MAX     32
#define TAB          4
#define DCLICK_TICKS 50
#define MSG_TICKS    400

enum { MODE_NONE = 0, MODE_SAVEAS, MODE_CONFIRM };
enum { KIND_NONE = 0, KIND_TYPE, KIND_DELETE, KIND_OTHER };
enum { DRAG_NONE = 0, DRAG_SELECT, DRAG_SCROLL };
enum { B_NEW, B_SAVE, B_SAVEAS, B_UNDO, B_REDO, B_CUT, B_COPY, B_PASTE, B_N };

typedef struct { char* text; int len; int cur; } snap_t;

typedef struct {
    wm_window_t* win;

    char*  buf;
    int    len, cap;
    int    cur, anc;                 // caret and the other end of the selection
    int*   ls;                       // offset at which each line starts
    int    nlines, ls_cap;

    int    top;                      // first visible line
    int    hscroll;                  // first visible column
    int    want_col;                 // column to hold across Up/Down, or -1
    int    vis_rows, vis_cols;       // text area size, set at paint

    uint32_t dir;                    // where the file lives
    char     name[FAT32_NAME_MAX];
    int      has_name;
    int      dirty;

    snap_t undo[UNDO_MAX]; int undo_n;
    snap_t redo[UNDO_MAX]; int redo_n;
    int    last_kind;

    int    drag, drag_off;
    unsigned long long click_when;
    int    click_count, click_x, click_y;

    int    mode;
    int    close_after;              // finish closing the window once saved
    int    overwrite_ok;             // the name in the Save As box may replace a file
    lineedit_t name_edit;

    char   msg[96];
    int    msg_err;
    unsigned long long msg_when;
} edit_t;

// ---------------------------------------------------------------- helpers

typedef struct { int x, y, w, h; } rect_t;
static int inside(rect_t r, int x, int y){
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static void say(edit_t* e, int err, const char* s){
    strncpy(e->msg, s, sizeof(e->msg) - 1);
    e->msg[sizeof(e->msg) - 1] = 0;
    e->msg_err = err;
    e->msg_when = ticks;
}

static int is_word(char c){
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static void sel_range(const edit_t* e, int* a, int* b){
    if (e->cur < e->anc){ *a = e->cur; *b = e->anc; } else { *a = e->anc; *b = e->cur; }
}

static int has_sel(const edit_t* e){ return e->cur != e->anc; }

// --------------------------------------------------------------- the buffer

static int ensure(edit_t* e, int need){
    if (need + 1 <= e->cap) return 0;
    int nc = e->cap * 2;
    if (nc < need + 1) nc = need + 1;
    if (nc < 4096) nc = 4096;
    char* nb = (char*)kmalloc((size_t)nc);
    if (!nb) return -1;
    if (e->buf){ memcpy(nb, e->buf, (size_t)e->len); kfree(e->buf); }
    e->buf = nb;
    e->cap = nc;
    return 0;
}

static void rebuild_lines(edit_t* e){
    int n = 1;
    for (int i = 0; i < e->len; i++) if (e->buf[i] == '\n') n++;
    if (n > e->ls_cap){
        int nc = n + 256;
        int* nl = (int*)kmalloc(sizeof(int) * (size_t)nc);
        if (nl){ if (e->ls) kfree(e->ls); e->ls = nl; e->ls_cap = nc; }
    }
    int k = 0;
    e->ls[k++] = 0;
    for (int i = 0; i < e->len && k < e->ls_cap; i++)
        if (e->buf[i] == '\n') e->ls[k++] = i + 1;
    e->nlines = k;
}

static int line_of(const edit_t* e, int pos){
    int lo = 0, hi = e->nlines - 1;
    while (lo < hi){
        int mid = (lo + hi + 1) / 2;
        if (e->ls[mid] <= pos) lo = mid; else hi = mid - 1;
    }
    return lo;
}

static int line_end(const edit_t* e, int ln){
    return (ln + 1 < e->nlines) ? e->ls[ln + 1] - 1 : e->len;
}

// The screen column of byte `pos` on line `ln`, with tabs expanded to the
// next stop.
static int vcol(const edit_t* e, int ln, int pos){
    int c = 0;
    for (int p = e->ls[ln]; p < pos; p++)
        c += (e->buf[p] == '\t') ? TAB - (c % TAB) : 1;
    return c;
}

// The byte offset (from the line start) nearest screen column `target`.
static int off_of_vcol(const edit_t* e, int ln, int target){
    int c = 0, p = e->ls[ln], end = line_end(e, ln);
    while (p < end){
        int w = (e->buf[p] == '\t') ? TAB - (c % TAB) : 1;
        if (target <= c + w / 2) break;
        c += w;
        p++;
    }
    return p - e->ls[ln];
}

static void ensure_visible(edit_t* e){
    int rows = e->vis_rows > 0 ? e->vis_rows : 1;
    int cols = e->vis_cols > 4 ? e->vis_cols : 4;
    int ln = line_of(e, e->cur);
    if (ln < e->top) e->top = ln;
    if (ln >= e->top + rows) e->top = ln - rows + 1;
    int vc = vcol(e, ln, e->cur);
    if (vc < e->hscroll) e->hscroll = vc;
    if (vc >= e->hscroll + cols - 1) e->hscroll = vc - cols + 2;
    if (e->hscroll < 0) e->hscroll = 0;
}

static void clamp_top(edit_t* e){
    int rows = e->vis_rows > 0 ? e->vis_rows : 1;
    int max = e->nlines - rows;
    if (max < 0) max = 0;
    if (e->top > max) e->top = max;
    if (e->top < 0) e->top = 0;
}

// ------------------------------------------------------------------- undo

static void snap_free_all(snap_t* st, int* n){
    for (int i = 0; i < *n; i++) if (st[i].text) kfree(st[i].text);
    *n = 0;
}

static int snap_take(const edit_t* e, snap_t* s){
    s->text = (char*)kmalloc((size_t)e->len + 1);
    if (!s->text) return -1;
    memcpy(s->text, e->buf, (size_t)e->len);
    s->len = e->len;
    s->cur = e->cur;
    return 0;
}

static void stack_push(snap_t* st, int* n, snap_t s){
    if (*n == UNDO_MAX){
        if (st[0].text) kfree(st[0].text);
        memmove(st, st + 1, sizeof(snap_t) * (UNDO_MAX - 1));
        (*n)--;
    }
    st[(*n)++] = s;
}

// Called before every edit. Consecutive edits of the same kind (a run of
// typing, a run of Backspace) share one snapshot; anything else, or a move of
// the caret in between, starts a new one.
static void undo_record(edit_t* e, int kind){
    if (kind != KIND_OTHER && kind == e->last_kind) return;
    e->last_kind = kind;
    snap_t s;
    if (snap_take(e, &s) == 0) stack_push(e->undo, &e->undo_n, s);
    snap_free_all(e->redo, &e->redo_n);
}

static void restore(edit_t* e, const snap_t* s){
    if (ensure(e, s->len) != 0) return;
    memcpy(e->buf, s->text, (size_t)s->len);
    e->len = s->len;
    e->cur = e->anc = s->cur;
    rebuild_lines(e);
    e->dirty = 1;
    e->want_col = -1;
    ensure_visible(e);
}

static void do_undo(edit_t* e){
    if (!e->undo_n) return;
    snap_t now;
    if (snap_take(e, &now) == 0) stack_push(e->redo, &e->redo_n, now);
    snap_t s = e->undo[--e->undo_n];
    restore(e, &s);
    kfree(s.text);
    e->last_kind = KIND_NONE;
}

static void do_redo(edit_t* e){
    if (!e->redo_n) return;
    snap_t now;
    if (snap_take(e, &now) == 0) stack_push(e->undo, &e->undo_n, now);
    snap_t s = e->redo[--e->redo_n];
    restore(e, &s);
    kfree(s.text);
    e->last_kind = KIND_NONE;
}

// ---------------------------------------------------------------- editing

// Replaces [a,b) with n bytes of s, leaving the caret after them. The one
// place the buffer changes, so the line index, dirty flag and scrolling are
// looked after exactly once.
static int replace(edit_t* e, int a, int b, const char* s, int n, int kind){
    if (e->len - (b - a) + n > (int)EDIT_MAX){
        say(e, 1, "The file is at the 128 KB limit");
        return -1;
    }
    if (ensure(e, e->len - (b - a) + n) != 0){ say(e, 1, "Out of memory"); return -1; }
    undo_record(e, kind);
    memmove(e->buf + a + n, e->buf + b, (size_t)(e->len - b));
    if (n) memcpy(e->buf + a, s, (size_t)n);
    e->len += n - (b - a);
    e->cur = e->anc = a + n;
    e->dirty = 1;
    e->want_col = -1;
    rebuild_lines(e);
    ensure_visible(e);
    return 0;
}

static void insert_bytes(edit_t* e, const char* s, int n, int kind){
    int a, b;
    sel_range(e, &a, &b);
    replace(e, a, b, s, n, kind);
}

static void move_to(edit_t* e, int pos, int extend, int keep_col){
    if (pos < 0) pos = 0;
    if (pos > e->len) pos = e->len;
    e->cur = pos;
    if (!extend) e->anc = pos;
    e->last_kind = KIND_NONE;
    if (!keep_col) e->want_col = -1;
    ensure_visible(e);
}

static void vertical(edit_t* e, int delta, int extend){
    int ln = line_of(e, e->cur);
    int t = ln + delta;
    if (t < 0){ move_to(e, 0, extend, 0); return; }
    if (t >= e->nlines){ move_to(e, e->len, extend, 0); return; }
    int col = e->want_col >= 0 ? e->want_col : vcol(e, ln, e->cur);
    int pos = e->ls[t] + off_of_vcol(e, t, col);
    move_to(e, pos, extend, 1);
    e->want_col = col;
}

static void copy_selection(edit_t* e){
    int a, b;
    sel_range(e, &a, &b);
    if (a != b){ clip_set_text(e->buf + a, (uint32_t)(b - a)); say(e, 0, "Copied"); }
}

static void cut_selection(edit_t* e){
    int a, b;
    sel_range(e, &a, &b);
    if (a == b) return;
    clip_set_text(e->buf + a, (uint32_t)(b - a));
    replace(e, a, b, 0, 0, KIND_OTHER);
}

static void paste(edit_t* e){
    uint32_t n;
    const char* t = clip_text(&n);
    if (!n){ say(e, 0, "The clipboard is empty"); return; }
    char* tmp = (char*)kmalloc(n + 1);
    if (!tmp) return;
    int k = 0;
    for (uint32_t i = 0; i < n; i++){
        char c = t[i];
        if (c == '\r') continue;
        if (c == '\n' || c == '\t' || (c >= 32 && c <= 126)) tmp[k++] = c;
    }
    if (k) insert_bytes(e, tmp, k, KIND_OTHER);
    kfree(tmp);
}

static void select_all(edit_t* e){
    e->anc = 0;
    e->cur = e->len;
    e->last_kind = KIND_NONE;
}

static void select_word(edit_t* e, int pos){
    int a = pos, b = pos;
    if (pos < e->len && is_word(e->buf[pos])){
        while (a > 0 && is_word(e->buf[a - 1])) a--;
        while (b < e->len && is_word(e->buf[b])) b++;
    } else if (pos < e->len && e->buf[pos] != '\n'){
        b = pos + 1;
    }
    e->anc = a; e->cur = b;
}

static void select_line(edit_t* e, int pos){
    int ln = line_of(e, pos);
    e->anc = e->ls[ln];
    e->cur = (ln + 1 < e->nlines) ? e->ls[ln + 1] : e->len;
}

static void newline(edit_t* e){
    // Carry the current line's indentation over, so code stays where it was.
    int a, b;
    sel_range(e, &a, &b);
    int ln = line_of(e, a);
    char tmp[64];
    int k = 0;
    tmp[k++] = '\n';
    for (int p = e->ls[ln]; p < a && k < (int)sizeof(tmp) - 1; p++){
        if (e->buf[p] == ' ' || e->buf[p] == '\t') tmp[k++] = e->buf[p]; else break;
    }
    replace(e, a, b, tmp, k, KIND_OTHER);
}

static void tab_key(edit_t* e){
    int a, b;
    sel_range(e, &a, &b);
    int ln = line_of(e, a);
    int n = TAB - (vcol(e, ln, a) % TAB);
    char sp[TAB];
    for (int i = 0; i < n; i++) sp[i] = ' ';
    replace(e, a, b, sp, n, KIND_TYPE);
}

static void backspace(edit_t* e){
    if (has_sel(e)){ insert_bytes(e, 0, 0, KIND_OTHER); return; }
    if (e->cur > 0) replace(e, e->cur - 1, e->cur, 0, 0, KIND_DELETE);
}

static void delete_fwd(edit_t* e){
    if (has_sel(e)){ insert_bytes(e, 0, 0, KIND_OTHER); return; }
    if (e->cur < e->len) replace(e, e->cur, e->cur + 1, 0, 0, KIND_DELETE);
}

// ------------------------------------------------------------- file access

static void update_title(edit_t* e){
    char t[WM_TITLE_MAX + 24];
    if (e->has_name){
        char nm[20];
        int nl = (int)strlen(e->name);
        if (nl > 15){ memcpy(nm, e->name, 14); nm[14] = '~'; nm[15] = 0; }
        else { strncpy(nm, e->name, sizeof(nm) - 1); nm[sizeof(nm) - 1] = 0; }
        ksnprintf(t, sizeof(t), "Editor - %s%s", nm, e->dirty ? " *" : "");
    } else {
        ksnprintf(t, sizeof(t), "Editor - Untitled%s", e->dirty ? " *" : "");
    }
    strncpy(e->win->title, t, WM_TITLE_MAX - 1);
    e->win->title[WM_TITLE_MAX - 1] = 0;
}

static int write_out(edit_t* e){
    if (fat32_write_file(e->dir, e->name, (const uint8_t*)e->buf, (uint32_t)e->len) != 0){
        say(e, 1, "Could not save: the disk is read-only or full");
        return -1;
    }
    e->dirty = 0;
    say(e, 0, "Saved");
    return 0;
}

static void begin_save_as(edit_t* e){
    e->mode = MODE_SAVEAS;
    e->overwrite_ok = 0;
    le_set(&e->name_edit, e->has_name ? e->name : "Untitled.txt", 1);
}

// Returns 1 if the buffer reached the disk, 0 if the user has been asked
// for a name instead, -1 on failure.
static int do_save(edit_t* e){
    if (!e->has_name){ begin_save_as(e); return 0; }
    return write_out(e) == 0 ? 1 : -1;
}

static void finish_close(edit_t* e){
    e->close_after = 0;
    wm_close(e->win);
}

static void commit_save_as(edit_t* e){
    const char* nm = e->name_edit.text;
    if (!fs_name_ok(nm)){ say(e, 1, "That name cannot be used (no \\ / : * ? \" < > |)"); return; }

    fat32_dirent_t ex;
    int same = e->has_name && kstricmp(nm, e->name) == 0;
    if (fs_exists(e->dir, nm, &ex) && !same){
        if (ex.is_dir){ say(e, 1, "A folder with that name already exists"); return; }
        if (!e->overwrite_ok){
            e->overwrite_ok = 1;
            say(e, 1, "That file exists - press Save again to replace it");
            return;
        }
    }

    char old[FAT32_NAME_MAX];
    int old_has = e->has_name;
    strncpy(old, e->name, sizeof(old) - 1); old[sizeof(old) - 1] = 0;
    strncpy(e->name, nm, sizeof(e->name) - 1);
    e->name[sizeof(e->name) - 1] = 0;
    e->has_name = 1;

    if (write_out(e) == 0){
        e->mode = MODE_NONE;
        if (e->close_after) finish_close(e);
    } else {
        // Put the old identity back: a failed Save As must not silently turn
        // the document into a different file.
        strncpy(e->name, old, sizeof(e->name) - 1);
        e->has_name = old_has;
    }
}

static int load_file(edit_t* e, uint32_t dir, const fat32_dirent_t* d){
    if (d->size > EDIT_MAX) return -1;
    if (ensure(e, (int)d->size + 1) != 0) return -3;
    uint32_t got = d->size ? fat32_read_file(d, (uint8_t*)e->buf, d->size) : 0;
    if (got != d->size) return -3;
    if (!fs_looks_like_text((const uint8_t*)e->buf, got)) return -2;

    // CRLF becomes LF: the buffer's lines end in one byte, and a stray CR
    // would otherwise show up as a glyph at the end of every line.
    int w = 0;
    for (uint32_t i = 0; i < got; i++){
        char c = e->buf[i];
        if (c == '\r'){ if (i + 1 < got && e->buf[i + 1] == '\n') continue; c = '\n'; }
        e->buf[w++] = c;
    }
    e->len = w;
    e->dir = dir;
    strncpy(e->name, d->name, sizeof(e->name) - 1);
    e->name[sizeof(e->name) - 1] = 0;
    e->has_name = 1;
    return 0;
}

// ------------------------------------------------------------------ layout

static const struct { const char* label; int gap; } BTN[B_N] = {
    { "New",     0 }, { "Save", 0 }, { "Save As", 0 },
    { "Undo",    1 }, { "Redo", 0 },
    { "Cut",     1 }, { "Copy", 0 }, { "Paste", 0 },
};

static rect_t button_rect(int i){
    int x = 8;
    for (int k = 0; k <= i; k++){
        if (BTN[k].gap) x += 12;
        int w = (int)gfx_text_width(BTN[k].label) + 20;
        if (k == i) return (rect_t){ x, 6, w, 26 };
        x += w + 4;
    }
    return (rect_t){ 0, 0, 0, 0 };
}

static int button_enabled(const edit_t* e, int id){
    switch (id){
        case B_UNDO:  return e->undo_n > 0;
        case B_REDO:  return e->redo_n > 0;
        case B_CUT:
        case B_COPY:  return has_sel(e);
        case B_PASTE: return clip_text_len() > 0;
        default:      return 1;
    }
}

typedef struct { rect_t r; const char* label; } bar_btn_t;

// The buttons in the bar under the toolbar (Save As / unsaved changes).
static int bar_buttons(const edit_t* e, int w, bar_btn_t out[3]){
    if (e->mode == MODE_SAVEAS){
        out[0] = (bar_btn_t){ { w - 12 - 76, 8, 76, 26 }, "Cancel" };
        out[1] = (bar_btn_t){ { w - 12 - 76 - 8 - 68, 8, 68, 26 }, "Save" };
        return 2;
    }
    out[0] = (bar_btn_t){ { w - 12 - 76, 8, 76, 26 }, "Cancel" };
    out[1] = (bar_btn_t){ { w - 12 - 76 - 8 - 116, 8, 116, 26 }, "Don't Save" };
    out[2] = (bar_btn_t){ { w - 12 - 76 - 8 - 116 - 8 - 68, 8, 68, 26 }, "Save" };
    return 3;
}

static int gutter_w(const edit_t* e){
    int d = 1;
    for (int n = e->nlines; n >= 10; n /= 10) d++;
    if (d < 3) d = 3;
    return (d + 2) * FONT_W;
}

// ---------------------------------------------------------------- painting

static void draw_button(rect_t r, const char* label, int enabled, int hot, int primary){
    uint32_t fill = primary ? TH_ACCENT : (hot && enabled ? TH_HOVER : TH_CONTROL);
    gfx_fill_round_rect((uint32_t)r.x, (uint32_t)r.y, (uint32_t)r.w, (uint32_t)r.h, 6, fill);
    if (!primary)
        gfx_draw_round_rect((uint32_t)r.x, (uint32_t)r.y, (uint32_t)r.w, (uint32_t)r.h, 6,
                            TH_CONTROL_EDGE);
    int tw = (int)gfx_text_width(label);
    uint32_t fg = primary ? TH_ACCENT_TEXT : (enabled ? TH_TEXT : TH_TEXT_DIM);
    gfx_text((uint32_t)(r.x + (r.w - tw) / 2), (uint32_t)(r.y + (r.h - 16) / 2), label, fg,
             GFX_TRANSPARENT);
}

static void paint_toolbar(edit_t* e, int x, int y, int w){
    gfx_fill_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, TOOLBAR_H, TH_PANEL);
    gfx_hline((uint32_t)x, (uint32_t)(y + TOOLBAR_H - 1), (uint32_t)w, TH_PANEL_EDGE);
    int mx = mouse_x(), my = mouse_y();
    for (int i = 0; i < B_N; i++){
        rect_t r = button_rect(i);
        r.x += x; r.y += y;
        draw_button(r, BTN[i].label, button_enabled(e, i), inside(r, mx, my), 0);
    }
}

static void paint_bar(edit_t* e, int x, int y, int w){
    gfx_fill_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, BAR_H, TH_SIDEBAR);
    gfx_hline((uint32_t)x, (uint32_t)(y + BAR_H - 1), (uint32_t)w, TH_PANEL_EDGE);

    bar_btn_t b[3];
    int n = bar_buttons(e, w, b);
    int mx = mouse_x(), my = mouse_y();

    if (e->mode == MODE_SAVEAS){
        gfx_text((uint32_t)(x + 12), (uint32_t)(y + 13), "Save as", TH_TEXT, GFX_TRANSPARENT);
        int fx = x + 12 + 7 * FONT_W + 12;
        int fw = b[1].r.x - 12 - (fx - x);
        if (fw < 40) fw = 40;
        gfx_fill_round_rect((uint32_t)fx, (uint32_t)(y + 8), (uint32_t)fw, 26, 6, TH_FIELD);
        gfx_draw_round_rect((uint32_t)fx, (uint32_t)(y + 8), (uint32_t)fw, 26, 6, TH_ACCENT);
        le_draw(&e->name_edit, fx + 8, y + 8, fw - 16, 26, 1);
    } else {
        char q[FAT32_NAME_MAX + 40];
        ksnprintf(q, sizeof(q), "Save changes to %s?", e->has_name ? e->name : "Untitled");
        gfx_clip_set((uint32_t)x, (uint32_t)y, (uint32_t)(b[2].r.x - 8), BAR_H);
        gfx_text((uint32_t)(x + 12), (uint32_t)(y + 13), q, TH_TEXT, GFX_TRANSPARENT);
        gfx_clip_reset();
    }
    for (int i = 0; i < n; i++){
        rect_t r = b[i].r;
        r.x += x; r.y += y;
        int primary = (e->mode == MODE_SAVEAS) ? (i == 1) : (i == 2);
        draw_button(r, b[i].label, 1, inside(r, mx, my), primary);
    }
}

static void paint_status(edit_t* e, int x, int y, int w){
    gfx_fill_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, STATUS_H, TH_PANEL);
    gfx_hline((uint32_t)x, (uint32_t)y, (uint32_t)w, TH_PANEL_EDGE);

    int ln = line_of(e, e->cur);
    char buf[96];
    ksnprintf(buf, sizeof(buf), "Ln %d, Col %d", ln + 1, vcol(e, ln, e->cur) + 1);
    gfx_text((uint32_t)(x + 12), (uint32_t)(y + 4), buf, TH_TEXT_MUTED, GFX_TRANSPARENT);

    int a, b;
    sel_range(e, &a, &b);
    if (a != b) ksnprintf(buf, sizeof(buf), "%d selected", b - a);
    else        ksnprintf(buf, sizeof(buf), "%d characters", e->len);
    gfx_text((uint32_t)(x + 140), (uint32_t)(y + 4), buf, TH_TEXT_MUTED, GFX_TRANSPARENT);

    const char* right;
    uint32_t col = TH_TEXT_MUTED;
    if (e->msg[0] && ticks - e->msg_when < MSG_TICKS){
        right = e->msg;
        col = e->msg_err ? TH_ERROR : TH_OK;
    } else {
        right = e->dirty ? "Modified" : (e->has_name ? "Saved" : "New file");
    }
    int rw = (int)gfx_text_width(right);
    int rx = x + w - 12 - rw;
    if (rx < x + 300) rx = x + 300;
    gfx_clip_set((uint32_t)(x + 300), (uint32_t)y, (uint32_t)(w - 300), STATUS_H);
    gfx_text((uint32_t)rx, (uint32_t)(y + 4), right, col, GFX_TRANSPARENT);
    gfx_clip_reset();
}

static char shown(char c){
    if (c < 32) return '.';
    if (c > 126) return '?';
    return c;
}

static void paint_text(edit_t* e, int x, int y, int w, int h){
    int gw = gutter_w(e);
    int tx = x + gw;
    int tw = w - gw - SCROLL_W;
    e->vis_rows = h / FONT_H;
    e->vis_cols = tw / FONT_W;
    if (e->vis_rows < 1) e->vis_rows = 1;
    clamp_top(e);

    gfx_fill_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, TH_FIELD);
    gfx_fill_rect((uint32_t)x, (uint32_t)y, (uint32_t)gw, (uint32_t)h, TH_PANEL);
    gfx_vline((uint32_t)(x + gw - 1), (uint32_t)y, (uint32_t)h, TH_PANEL_EDGE);

    int a, b;
    sel_range(e, &a, &b);
    int cur_ln = line_of(e, e->cur);

    for (int r = 0; r < e->vis_rows; r++){
        int ln = e->top + r;
        if (ln >= e->nlines) break;
        int ry = y + r * FONT_H;

        if (ln == cur_ln && a == b)
            gfx_fill_rect((uint32_t)tx, (uint32_t)ry, (uint32_t)(tw), FONT_H, TH_PANEL);

        char num[12];
        ksnprintf(num, sizeof(num), "%d", ln + 1);
        int nw = (int)strlen(num) * FONT_W;
        gfx_text((uint32_t)(x + gw - 10 - nw), (uint32_t)ry, num,
                 ln == cur_ln ? TH_TEXT : TH_TEXT_DIM, GFX_TRANSPARENT);

        int ls = e->ls[ln], le_ = line_end(e, ln);

        // Selection first, so the glyphs land on top of it.
        if (a != b && b > ls && a <= le_){
            int s0 = a > ls ? a : ls;
            int s1 = b < le_ ? b : le_;
            int c0 = vcol(e, ln, s0) - e->hscroll;
            int c1 = vcol(e, ln, s1) - e->hscroll;
            if (b > le_) c1++;                    // the newline itself is selected
            if (c0 < 0) c0 = 0;
            if (c1 > e->vis_cols) c1 = e->vis_cols;
            if (c1 > c0)
                gfx_fill_rect((uint32_t)(tx + c0 * FONT_W), (uint32_t)ry,
                              (uint32_t)((c1 - c0) * FONT_W), FONT_H, TH_ACCENT_DIM);
        }

        gfx_clip_set((uint32_t)tx, (uint32_t)y, (uint32_t)tw, (uint32_t)h);
        int c = 0;
        for (int p = ls; p < le_; p++){
            char ch = e->buf[p];
            int cw = (ch == '\t') ? TAB - (c % TAB) : 1;
            int sc = c - e->hscroll;
            if (sc >= e->vis_cols) break;
            if (ch != '\t' && ch != ' ' && sc + cw > 0)
                gfx_char16((uint32_t)(tx + sc * FONT_W), (uint32_t)ry, shown(ch), TH_TEXT,
                           GFX_TRANSPARENT);
            c += cw;
        }
        gfx_clip_reset();

        if (ln == cur_ln){
            int sc = vcol(e, ln, e->cur) - e->hscroll;
            if (sc >= 0 && sc <= e->vis_cols && wm_is_focused(e->win) && e->mode != MODE_SAVEAS)
                gfx_fill_rect((uint32_t)(tx + sc * FONT_W), (uint32_t)ry, 2, FONT_H, TH_ACCENT);
        }
    }

    // Scroll bar: a track with a thumb sized to the fraction of the file in
    // view, drawn only when there is something to scroll.
    int sx = x + w - SCROLL_W;
    gfx_fill_rect((uint32_t)sx, (uint32_t)y, SCROLL_W, (uint32_t)h, TH_PANEL);
    if (e->nlines > e->vis_rows){
        int th_ = h * e->vis_rows / e->nlines;
        if (th_ < 24) th_ = 24;
        int ty = y + (h - th_) * e->top / (e->nlines - e->vis_rows);
        gfx_fill_round_rect((uint32_t)(sx + 2), (uint32_t)ty, SCROLL_W - 4, (uint32_t)th_, 4,
                            TH_TEXT_DIM);
    }
}

static void paint(wm_window_t* win, edit_t* e){
    int x, y, w, h;
    wm_client_rect(win, &x, &y, &w, &h);

    paint_toolbar(e, x, y, w);
    int by = y + TOOLBAR_H;
    if (e->mode != MODE_NONE){ paint_bar(e, x, by, w); by += BAR_H; }
    paint_text(e, x, by, w, y + h - STATUS_H - by);
    paint_status(e, x, y + h - STATUS_H, w);
}

// ------------------------------------------------------------------- input

// Where the text area is, in client coordinates.
static void text_area(const edit_t* e, int w, int h, int* x, int* y, int* tw, int* th_){
    int by = TOOLBAR_H + (e->mode != MODE_NONE ? BAR_H : 0);
    *x = gutter_w(e);
    *y = by;
    *tw = w - *x - SCROLL_W;
    *th_ = h - STATUS_H - by;
}

static int pos_at(const edit_t* e, int px, int py){
    int ln = e->top + (py >= 0 ? py / FONT_H : -1 + py / FONT_H);
    if (ln < 0) ln = 0;
    if (ln >= e->nlines) ln = e->nlines - 1;
    int col = (px < 0 ? 0 : (px + FONT_W / 2) / FONT_W) + e->hscroll;
    return e->ls[ln] + off_of_vcol(e, ln, col);
}

static void run_button(edit_t* e, int id){
    switch (id){
        case B_NEW:    app_edit_open(); break;
        case B_SAVE:   do_save(e); break;
        case B_SAVEAS: begin_save_as(e); break;
        case B_UNDO:   do_undo(e); break;
        case B_REDO:   do_redo(e); break;
        case B_CUT:    cut_selection(e); break;
        case B_COPY:   copy_selection(e); break;
        case B_PASTE:  paste(e); break;
        default: break;
    }
}

static void bar_press(edit_t* e, int idx){
    if (e->mode == MODE_SAVEAS){
        if (idx == 1) commit_save_as(e);
        else { e->mode = MODE_NONE; e->close_after = 0; }
        return;
    }
    // Unsaved-changes prompt: Save, Don't Save, Cancel.
    if (idx == 2){
        e->close_after = 1;
        int r = do_save(e);
        if (r == 1) finish_close(e);
        else if (r < 0) e->close_after = 0;
        else if (e->mode == MODE_CONFIRM) e->mode = MODE_SAVEAS;
    } else if (idx == 1){
        e->dirty = 0;
        finish_close(e);
    } else {
        e->mode = MODE_NONE;
    }
}

static void on_mouse_down(edit_t* e, int cx, int cy, int w, int h){
    if (cy < TOOLBAR_H){
        for (int i = 0; i < B_N; i++)
            if (inside(button_rect(i), cx, cy) && button_enabled(e, i)){
                run_button(e, i);
                return;
            }
        return;
    }
    if (e->mode != MODE_NONE && cy < TOOLBAR_H + BAR_H){
        bar_btn_t b[3];
        int n = bar_buttons(e, w, b);
        int ry = cy - TOOLBAR_H;
        for (int i = 0; i < n; i++)
            if (inside(b[i].r, cx, ry)){ bar_press(e, i); return; }
        if (e->mode == MODE_SAVEAS){
            int fx = 12 + 7 * FONT_W + 12;
            if (cx >= fx + 8) le_click(&e->name_edit, cx - (fx + 8), kbd_shift_down());
        }
        return;
    }
    if (cy >= h - STATUS_H) return;

    int tx, ty, tw, th_;
    text_area(e, w, h, &tx, &ty, &tw, &th_);

    // Scroll bar
    if (cx >= w - SCROLL_W){
        if (e->nlines > e->vis_rows){
            int thumb = th_ * e->vis_rows / e->nlines;
            if (thumb < 24) thumb = 24;
            int top_px = ty + (th_ - thumb) * e->top / (e->nlines - e->vis_rows);
            if (cy >= top_px && cy < top_px + thumb){
                e->drag = DRAG_SCROLL;
                e->drag_off = cy - top_px;
            } else {
                e->top += (cy < top_px ? -1 : 1) * e->vis_rows;
                clamp_top(e);
            }
        }
        return;
    }

    if (e->mode == MODE_CONFIRM) return;          // the prompt owns the keyboard
    if (cx < tx) return;                          // gutter

    int pos = pos_at(e, cx - tx, cy - ty);
    int dbl = (ticks - e->click_when < DCLICK_TICKS &&
               cx - e->click_x < 5 && e->click_x - cx < 5 &&
               cy - e->click_y < 5 && e->click_y - cy < 5);
    e->click_count = dbl ? e->click_count + 1 : 1;
    e->click_when = ticks; e->click_x = cx; e->click_y = cy;

    if (e->mode == MODE_SAVEAS) e->mode = MODE_NONE;   // clicking the text abandons the prompt

    if (e->click_count == 2) select_word(e, pos);
    else if (e->click_count >= 3){ select_line(e, pos); e->click_count = 0; }
    else move_to(e, pos, kbd_shift_down(), 0);
    e->last_kind = KIND_NONE;
    e->drag = DRAG_SELECT;
}

static void on_mouse_move(edit_t* e, int cx, int cy, int w, int h){
    int tx, ty, tw, th_;
    text_area(e, w, h, &tx, &ty, &tw, &th_);

    if (e->drag == DRAG_SELECT){
        // Dragging past the top or bottom edge scrolls, one line per motion
        // event, so the selection can reach text that is out of view.
        if (cy < ty) e->top--;
        else if (cy >= ty + th_) e->top++;
        clamp_top(e);
        if (cx < tx) e->hscroll = e->hscroll > 0 ? e->hscroll - 1 : 0;
        else if (cx >= tx + tw) e->hscroll++;
        e->cur = pos_at(e, cx - tx, cy - ty);
        e->last_kind = KIND_NONE;
    } else if (e->drag == DRAG_SCROLL && e->nlines > e->vis_rows){
        int thumb = th_ * e->vis_rows / e->nlines;
        if (thumb < 24) thumb = 24;
        int span = th_ - thumb;
        if (span < 1) span = 1;
        e->top = (cy - ty - e->drag_off) * (e->nlines - e->vis_rows) / span;
        clamp_top(e);
    }
}

static void on_key(edit_t* e, int key){
    int ctrl = kbd_ctrl_down(), shift = kbd_shift_down();

    // Prompts take the keyboard first.
    if (e->mode == MODE_SAVEAS){
        if (key == '\n'){ commit_save_as(e); return; }
        if (key == 27){ e->mode = MODE_NONE; e->close_after = 0; return; }
        e->overwrite_ok = 0;
        le_key(&e->name_edit, key);
        return;
    }
    if (e->mode == MODE_CONFIRM){
        if (key == '\n') bar_press(e, 2);
        else if (key == 27) bar_press(e, 0);
        return;
    }

    if (ctrl){
        int k = (key >= 'A' && key <= 'Z') ? key + 32 : key;
        switch (k){
            case 's': if (shift) begin_save_as(e); else do_save(e); return;
            case 'n': app_edit_open(); return;
            case 'w': wm_request_close(e->win); return;
            case 'a': select_all(e); return;
            case 'c': copy_selection(e); return;
            case 'x': cut_selection(e); return;
            case 'v': paste(e); return;
            case 'z': if (shift) do_redo(e); else do_undo(e); return;
            case 'y': do_redo(e); return;
            default: break;
        }
        if (key == KEY_HOME){ move_to(e, 0, shift, 0); return; }
        if (key == KEY_END){ move_to(e, e->len, shift, 0); return; }
        return;
    }

    int ln = line_of(e, e->cur);
    switch (key){
        case KEY_LEFT:
            if (has_sel(e) && !shift){ int a, b; sel_range(e, &a, &b); move_to(e, a, 0, 0); }
            else move_to(e, e->cur - 1, shift, 0);
            break;
        case KEY_RIGHT:
            if (has_sel(e) && !shift){ int a, b; sel_range(e, &a, &b); move_to(e, b, 0, 0); }
            else move_to(e, e->cur + 1, shift, 0);
            break;
        case KEY_UP:    vertical(e, -1, shift); break;
        case KEY_DOWN:  vertical(e,  1, shift); break;
        case KEY_PGUP:  vertical(e, -(e->vis_rows > 1 ? e->vis_rows - 1 : 1), shift); break;
        case KEY_PGDN:  vertical(e,  (e->vis_rows > 1 ? e->vis_rows - 1 : 1), shift); break;
        case KEY_HOME:  move_to(e, e->ls[ln], shift, 0); break;
        case KEY_END:   move_to(e, line_end(e, ln), shift, 0); break;
        case KEY_DELETE: delete_fwd(e); break;
        case '\b':      backspace(e); break;
        case '\n':      newline(e); break;
        case '\t':      tab_key(e); break;
        case 27:        if (has_sel(e)) move_to(e, e->cur, 0, 0); break;
        default:
            if (key >= 32 && key <= 126){ char c = (char)key; insert_bytes(e, &c, 1, KIND_TYPE); }
            break;
    }
}

static void free_all(edit_t* e){
    snap_free_all(e->undo, &e->undo_n);
    snap_free_all(e->redo, &e->redo_n);
    if (e->buf) kfree(e->buf);
    if (e->ls) kfree(e->ls);
    kfree(e);
}

static void handler(wm_window_t* win, const wm_event_t* ev){
    edit_t* e = (edit_t*)win->user;
    if (!e) return;

    int x, y, w, h;
    wm_client_rect(win, &x, &y, &w, &h);

    switch (ev->type){
        case WM_EV_PAINT:
            update_title(e);
            paint(win, e);
            break;

        case WM_EV_KEY:
            on_key(e, ev->key);
            if (win->user) update_title(e);      // the key may have closed the window
            wm_invalidate();
            break;

        case WM_EV_WHEEL:
            e->top += ev->key * 3;
            clamp_top(e);
            wm_invalidate();
            break;

        case WM_EV_MOUSE_DOWN:
            on_mouse_down(e, ev->x, ev->y, w, h);
            if (win->user) update_title(e);
            wm_invalidate();
            break;

        case WM_EV_MOUSE_MOVE:
            on_mouse_move(e, ev->x, ev->y, w, h);
            wm_invalidate();
            break;

        case WM_EV_MOUSE_UP:
            e->drag = DRAG_NONE;
            break;

        case WM_EV_TICK:
            // A status message that has run its course is cleared with a
            // repaint; otherwise nothing here needs to draw on a timer.
            if (e->msg[0] && ticks - e->msg_when >= MSG_TICKS){ e->msg[0] = 0; wm_invalidate(); }
            break;

        case WM_EV_CLOSE_REQ:
            if (e->dirty && e->mode != MODE_CONFIRM){
                e->mode = MODE_CONFIRM;
                wm_focus(win);
                win->keep_open = 1;
                wm_invalidate();
            } else if (e->dirty){
                win->keep_open = 1;       // already asking
            }
            break;

        case WM_EV_CLOSE:
            free_all(e);
            win->user = 0;
            break;

        default: break;
    }
}

// ------------------------------------------------------------------- entry

static edit_t* create(void){
    edit_t* e = (edit_t*)kmalloc(sizeof(edit_t));
    if (!e) return 0;
    memset(e, 0, sizeof(*e));
    e->want_col = -1;
    e->vis_rows = 20;
    e->vis_cols = 80;
    e->click_count = 0;
    shell_init();
    e->dir = fat32_root_cluster();
    if (ensure(e, 0) != 0){ kfree(e); return 0; }
    rebuild_lines(e);
    return e;
}

static void open_window(edit_t* e, const char* title){
    wm_window_t* w = wm_open(title, 160, 70, 760, 520, handler, e);
    if (!w){ free_all(e); return; }
    e->win = w;
    wm_set_wheel(w, 1);
    update_title(e);
}

void app_edit_open(void){
    edit_t* e = create();
    if (e) open_window(e, "Editor - Untitled");
}

int app_edit_open_file(uint32_t dir, const fat32_dirent_t* d){
    edit_t* e = create();
    if (!e) return -3;
    int r = load_file(e, dir, d);
    if (r != 0){ free_all(e); return r; }
    rebuild_lines(e);
    open_window(e, "Editor");
    return 0;
}
