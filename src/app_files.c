// src/app_files.c — Files
//
// Laid out the way Windows Explorer is, because that is the arrangement most
// people already know how to use: a navigation pane down the left, a details
// list in the middle with a column header, a breadcrumb address bar and
// back/forward/up buttons across the top, and a status line at the bottom.
// A preview pane appears on the right when a file is selected and gets out of
// the way when one is not.
//
// The previous version was two panes -- a raw directory listing beside a hex
// dump -- addressed by cluster number. Everything here that looks like polish
// is really about that: a path instead of a cluster, folders sorted to the
// top, "." and ".." hidden because there are buttons for going up, and a type
// column so a listing says what its entries are.
#include <stdint.h>
#include "header/apps.h"
#include "header/wm.h"
#include "header/theme.h"
#include "header/gfx.h"
#include "header/fat32.h"
#include "header/kheap.h"
#include "header/kstring.h"
#include "header/kbd.h"
#include "header/shell.h"
#include "header/font.h"

extern volatile unsigned long long ticks;

#define MAX_ENTRIES  128
#define PREVIEW_CAP  (8u * 1024u)

#define ROW_H        20
#define NAV_W        158
#define PREVIEW_W    230
#define TOOLBAR_H    40
#define HEADER_H     24
#define STATUS_H     24

#define COL_TYPE     280        // from the left edge of the list pane
#define COL_SIZE     420

// Deep enough for any tree this filesystem is likely to hold, and a fixed
// array means navigation can never fail on an allocation.
#define DEPTH_MAX 12
#define NAV_MAX   10

// The PIT runs at 100 Hz. Windows' own default double-click time is 500 ms.
#define DCLICK_TICKS 50

typedef struct {
    uint32_t       cluster;
    fat32_dirent_t entries[MAX_ENTRIES];
    int            count;
    int            sel;
    int            scroll;

    // Ancestors, for the Up button and the breadcrumb. The name is kept
    // beside the cluster because a cluster number is not something anybody
    // wants to read at the top of a window.
    uint32_t       stack[DEPTH_MAX];
    char           names[DEPTH_MAX][FAT32_NAME_MAX];
    int            depth;

    // Where Back returns to. Forward is deliberately absent as a history:
    // the button is drawn disabled, because a Forward that silently did
    // nothing would be worse than one that says it cannot.
    uint32_t       back_cluster[DEPTH_MAX];
    int            back_depth[DEPTH_MAX];
    int            back_n;

    fat32_dirent_t nav[NAV_MAX];        // top-level folders, for the sidebar
    int            nav_n;

    uint8_t*       preview;
    uint32_t       preview_len;
    int            preview_is_dir;

    int            mounted;
    unsigned long long click_when;
    int            click_row;
} files_t;

static int is_dot(const char* n){ return n[0] == '.' && n[1] == 0; }
static int is_dotdot(const char* n){ return n[0] == '.' && n[1] == '.' && n[2] == 0; }

static void load_preview(files_t* f);

// ----------------------------------------------------------------- model

// Folders first, then alphabetically. A raw FAT directory comes back in
// creation order, which puts a listing in no order anybody can navigate.
static void sort_entries(files_t* f){
    for (int i = 1; i < f->count; i++){
        fat32_dirent_t key = f->entries[i];
        int j = i - 1;
        while (j >= 0){
            const fat32_dirent_t* a = &f->entries[j];
            int worse = 0;
            if (a->is_dir != key.is_dir) worse = (!a->is_dir && key.is_dir);
            else worse = (kstricmp(a->name, key.name) > 0);
            if (!worse) break;
            f->entries[j + 1] = f->entries[j];
            j--;
        }
        f->entries[j + 1] = key;
    }
}

static void load_dir(files_t* f, uint32_t cluster){
    fat32_dirent_t raw[MAX_ENTRIES];
    int n = fat32_list(cluster, raw, MAX_ENTRIES);
    if (n < 0) n = 0;

    // "." and ".." are filesystem bookkeeping. There is an Up button and a
    // breadcrumb for what they were used for, and leaving them in the list
    // means the first two rows of every folder are not files.
    f->count = 0;
    for (int i = 0; i < n && f->count < MAX_ENTRIES; i++){
        if (is_dot(raw[i].name) || is_dotdot(raw[i].name)) continue;
        f->entries[f->count++] = raw[i];
    }

    f->cluster = cluster;
    f->sel     = f->count ? 0 : -1;
    f->scroll  = 0;
    sort_entries(f);
    load_preview(f);
}

static void load_nav(files_t* f){
    fat32_dirent_t raw[MAX_ENTRIES];
    int n = fat32_list(fat32_root_cluster(), raw, MAX_ENTRIES);
    f->nav_n = 0;
    for (int i = 0; i < n && f->nav_n < NAV_MAX; i++){
        if (!raw[i].is_dir || is_dot(raw[i].name) || is_dotdot(raw[i].name)) continue;
        f->nav[f->nav_n++] = raw[i];
    }
}

static void load_preview(files_t* f){
    f->preview_len    = 0;
    f->preview_is_dir = 0;
    if (f->sel < 0 || f->sel >= f->count || !f->preview) return;

    const fat32_dirent_t* e = &f->entries[f->sel];
    if (e->is_dir){ f->preview_is_dir = 1; return; }

    uint32_t cap = e->size < PREVIEW_CAP ? e->size : PREVIEW_CAP;
    f->preview_len = fat32_read_file(e, f->preview, cap);
}

static void push_back(files_t* f){
    if (f->back_n >= DEPTH_MAX) return;
    f->back_cluster[f->back_n] = f->cluster;
    f->back_depth[f->back_n]   = f->depth;
    f->back_n++;
}

static void go_into(files_t* f, const fat32_dirent_t* e){
    if (f->depth >= DEPTH_MAX) return;
    push_back(f);
    f->stack[f->depth] = f->cluster;
    strncpy(f->names[f->depth], e->name, FAT32_NAME_MAX - 1);
    f->names[f->depth][FAT32_NAME_MAX - 1] = 0;
    f->depth++;
    // A directory entry whose first cluster is 0 means the root; FAT32
    // stores the root's parent that way in every ".." entry.
    load_dir(f, e->first_cluster ? e->first_cluster : fat32_root_cluster());
}

static void go_up(files_t* f){
    if (f->depth <= 0) return;
    push_back(f);
    load_dir(f, f->stack[--f->depth]);
}

static void go_back(files_t* f){
    if (f->back_n <= 0) return;
    f->back_n--;
    f->depth = f->back_depth[f->back_n];
    load_dir(f, f->back_cluster[f->back_n]);
}

static void go_root(files_t* f){
    push_back(f);
    f->depth = 0;
    load_dir(f, fat32_root_cluster());
}

static void enter_selected(files_t* f){
    if (f->sel < 0 || f->sel >= f->count) return;
    const fat32_dirent_t* e = &f->entries[f->sel];
    if (e->is_dir) go_into(f, e);
}

// ------------------------------------------------------------- formatting

static const char* ext_of(const char* name){
    const char* dot = 0;
    for (const char* p = name; *p; p++) if (*p == '.') dot = p;
    return dot ? dot + 1 : 0;
}

static const char* type_of(const fat32_dirent_t* e, char* buf, uint32_t cap){
    if (e->is_dir) return "File folder";
    const char* x = ext_of(e->name);
    if (!x || !*x) return "File";
    if (kstricmp(x, "TXT") == 0)  return "Text Document";
    if (kstricmp(x, "MD") == 0)   return "Markdown File";
    if (kstricmp(x, "HTM") == 0 || kstricmp(x, "HTML") == 0) return "HTML Document";
    if (kstricmp(x, "C") == 0 || kstricmp(x, "H") == 0) return "C Source File";
    if (kstricmp(x, "BIN") == 0)  return "Binary File";
    if (kstricmp(x, "CFG") == 0 || kstricmp(x, "INI") == 0) return "Configuration";
    ksnprintf(buf, cap, "%s File", x);
    return buf;
}

// Sizes the way a file manager writes them: whole KB above a kilobyte, and
// never a fraction, because a column of "1.4 KB" and "17 bytes" does not
// line up and is not easier to read for it.
static void size_str(uint32_t bytes, char* buf, uint32_t cap){
    if (bytes >= 1024u * 1024u) ksnprintf(buf, cap, "%u MB", bytes / (1024u * 1024u));
    else if (bytes >= 1024u)    ksnprintf(buf, cap, "%u KB", (bytes + 1023u) / 1024u);
    else                        ksnprintf(buf, cap, "%u bytes", bytes);
}

// ---------------------------------------------------------------- glyphs

static void icon_folder(int x, int y){
    uint32_t back = GFX_RGB(0xD9, 0xA1, 0x2C), front = GFX_RGB(0xF2, 0xC2, 0x54);
    gfx_fill_rect((uint32_t)x, (uint32_t)(y + 1), 7, 3, back);
    gfx_fill_rect((uint32_t)x, (uint32_t)(y + 3), 15, 9, front);
    gfx_hline((uint32_t)x, (uint32_t)(y + 3), 15, back);
}

static void icon_file(int x, int y){
    gfx_fill_rect((uint32_t)(x + 2), (uint32_t)y, 11, 13, TH_FIELD);
    gfx_draw_rect((uint32_t)(x + 2), (uint32_t)y, 11, 13, TH_TEXT_DIM);
    // The folded corner, which is what makes eleven by thirteen read as paper.
    gfx_fill_rect((uint32_t)(x + 9), (uint32_t)y, 4, 4, TH_PANEL);
    for (int i = 0; i < 4; i++)
        gfx_put_pixel((uint32_t)(x + 9 + i), (uint32_t)(y + i), TH_TEXT_DIM);
    gfx_hline((uint32_t)(x + 4), (uint32_t)(y + 7), 7, TH_TEXT_DIM);
    gfx_hline((uint32_t)(x + 4), (uint32_t)(y + 10), 5, TH_TEXT_DIM);
}

// A chevron, for the toolbar buttons and the breadcrumb separators.
// (x, y) is the tip and `dir` is the way it points: -1 for "<", +1 for ">".
// The arms sweep away from the tip, opposite to the direction it faces.
static void chevron(int x, int y, int dir, uint32_t c){
    for (int i = 0; i < 4; i++){
        gfx_fill_rect((uint32_t)(x - dir * i), (uint32_t)(y - i), 2, 2, c);
        gfx_fill_rect((uint32_t)(x - dir * i), (uint32_t)(y + i), 2, 2, c);
    }
}

// ---------------------------------------------------------------- painting

static void paint_toolbar(files_t* f, int x, int y, int w){
    gfx_fill_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, TOOLBAR_H, TH_PANEL);
    gfx_hline((uint32_t)x, (uint32_t)(y + TOOLBAR_H - 1), (uint32_t)w, TH_PANEL_EDGE);

    int by = y + 9;
    uint32_t back_c = f->back_n ? TH_TEXT : TH_TEXT_DIM;
    uint32_t up_c   = f->depth  ? TH_TEXT : TH_TEXT_DIM;

    chevron(x + 20, by + 11, -1, back_c);
    chevron(x + 48, by + 11,  1, TH_TEXT_DIM);         // Forward: never armed

    // Up: the same chevron turned a quarter turn, with a stem under it.
    for (int i = 0; i < 4; i++){
        gfx_fill_rect((uint32_t)(x + 72 - i), (uint32_t)(by + 8 + i), 2, 2, up_c);
        gfx_fill_rect((uint32_t)(x + 72 + i), (uint32_t)(by + 8 + i), 2, 2, up_c);
    }
    gfx_fill_rect((uint32_t)(x + 72), (uint32_t)(by + 8), 2, 9, up_c);

    // Address bar: a breadcrumb, so the window says where it is rather than
    // which cluster it is reading.
    int ax = x + 92, aw = w - 92 - 12;
    gfx_fill_round_rect((uint32_t)ax, (uint32_t)by, (uint32_t)aw, 22, 4, TH_FIELD);
    gfx_draw_round_rect((uint32_t)ax, (uint32_t)by, (uint32_t)aw, 22, 4, TH_CONTROL_EDGE);

    gfx_clip_set((uint32_t)(ax + 2), (uint32_t)by, (uint32_t)(aw - 4), 22);
    int tx = ax + 10;
    gfx_text((uint32_t)tx, (uint32_t)(by + 3), "hawkOS (C:)", TH_TEXT, GFX_TRANSPARENT);
    tx += (int)gfx_text_width("hawkOS (C:)");
    for (int d = 0; d < f->depth; d++){
        chevron(tx + 8, by + 11, 1, TH_TEXT_DIM);
        tx += 20;
        gfx_text((uint32_t)tx, (uint32_t)(by + 3), f->names[d], TH_TEXT, GFX_TRANSPARENT);
        tx += (int)gfx_text_width(f->names[d]);
    }
    gfx_clip_reset();
}

static void paint_nav(files_t* f, int x, int y, int w, int h){
    gfx_fill_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, TH_SIDEBAR);
    gfx_vline((uint32_t)(x + w - 1), (uint32_t)y, (uint32_t)h, TH_PANEL_EDGE);

    gfx_text((uint32_t)(x + 12), (uint32_t)(y + 8), "This PC", TH_TEXT_MUTED, GFX_TRANSPARENT);

    int ry = y + 32;
    int at_root = (f->depth == 0);
    if (at_root)
        gfx_fill_round_rect((uint32_t)(x + 4), (uint32_t)(ry - 2), (uint32_t)(w - 10),
                            ROW_H, 5, TH_SELECT);
    icon_folder(x + 12, ry + 2);
    gfx_text((uint32_t)(x + 34), (uint32_t)ry, "hawkOS (C:)",
             at_root ? TH_SELECT_TEXT : TH_TEXT, GFX_TRANSPARENT);

    for (int i = 0; i < f->nav_n; i++){
        ry = y + 32 + (i + 1) * (ROW_H + 2);
        if (ry + ROW_H > y + h) break;
        int on = (f->depth == 1 && kstricmp(f->names[0], f->nav[i].name) == 0);
        if (on)
            gfx_fill_round_rect((uint32_t)(x + 4), (uint32_t)(ry - 2), (uint32_t)(w - 10),
                                ROW_H, 5, TH_SELECT);
        icon_folder(x + 28, ry + 2);
        gfx_text((uint32_t)(x + 50), (uint32_t)ry, f->nav[i].name,
                 on ? TH_SELECT_TEXT : TH_TEXT, GFX_TRANSPARENT);
    }
}

static void paint_header(int x, int y, int w){
    gfx_fill_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, HEADER_H, TH_PANEL);
    gfx_hline((uint32_t)x, (uint32_t)(y + HEADER_H - 1), (uint32_t)w, TH_PANEL_EDGE);
    gfx_text((uint32_t)(x + 34), (uint32_t)(y + 4), "Name", TH_TEXT_MUTED, GFX_TRANSPARENT);
    if (w > COL_TYPE + 60){
        gfx_vline((uint32_t)(x + COL_TYPE - 12), (uint32_t)(y + 5), HEADER_H - 12, TH_PANEL_EDGE);
        gfx_text((uint32_t)(x + COL_TYPE), (uint32_t)(y + 4), "Type", TH_TEXT_MUTED, GFX_TRANSPARENT);
    }
    if (w > COL_SIZE + 60){
        gfx_vline((uint32_t)(x + COL_SIZE - 12), (uint32_t)(y + 5), HEADER_H - 12, TH_PANEL_EDGE);
        gfx_text((uint32_t)(x + COL_SIZE), (uint32_t)(y + 4), "Size", TH_TEXT_MUTED, GFX_TRANSPARENT);
    }
}

static void paint_list(files_t* f, int x, int y, int w, int h){
    gfx_fill_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, TH_FIELD);

    if (!f->mounted){
        gfx_text((uint32_t)(x + 16), (uint32_t)(y + 18), "No volume mounted",
                 TH_TEXT, GFX_TRANSPARENT);
        gfx_text((uint32_t)(x + 16), (uint32_t)(y + 40),
                 "There is no FAT32 disk on the ATA channel.",
                 TH_TEXT_MUTED, GFX_TRANSPARENT);
        return;
    }
    if (!f->count){
        gfx_text((uint32_t)(x + 16), (uint32_t)(y + 18), "This folder is empty",
                 TH_TEXT_MUTED, GFX_TRANSPARENT);
        return;
    }

    int visible = h / ROW_H;
    if (f->sel >= 0){
        if (f->sel < f->scroll) f->scroll = f->sel;
        if (f->sel >= f->scroll + visible) f->scroll = f->sel - visible + 1;
    }

    char tbuf[24], sbuf[24];
    for (int i = 0; i < visible && f->scroll + i < f->count; i++){
        int idx = f->scroll + i;
        int ry  = y + i * ROW_H;
        const fat32_dirent_t* e = &f->entries[idx];
        int on = (idx == f->sel);

        if (on) gfx_fill_rect((uint32_t)x, (uint32_t)ry, (uint32_t)w, ROW_H, TH_SELECT);
        uint32_t fg = on ? TH_SELECT_TEXT : TH_TEXT;

        if (e->is_dir) icon_folder(x + 10, ry + 3);
        else           icon_file(x + 10, ry + 3);

        gfx_text((uint32_t)(x + 34), (uint32_t)(ry + 2), e->name, fg, GFX_TRANSPARENT);

        if (w > COL_TYPE + 60)
            gfx_text((uint32_t)(x + COL_TYPE), (uint32_t)(ry + 2),
                     type_of(e, tbuf, sizeof(tbuf)),
                     on ? TH_SELECT_TEXT : TH_TEXT_MUTED, GFX_TRANSPARENT);

        if (w > COL_SIZE + 60 && !e->is_dir){
            size_str(e->size, sbuf, sizeof(sbuf));
            gfx_text((uint32_t)(x + COL_SIZE), (uint32_t)(ry + 2), sbuf,
                     on ? TH_SELECT_TEXT : TH_TEXT_MUTED, GFX_TRANSPARENT);
        }
    }
}

static void paint_preview(files_t* f, int x, int y, int w, int h){
    gfx_fill_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, TH_PANEL);
    gfx_vline((uint32_t)x, (uint32_t)y, (uint32_t)h, TH_PANEL_EDGE);

    const fat32_dirent_t* e = &f->entries[f->sel];
    char buf[32];

    gfx_clip_set((uint32_t)(x + 10), (uint32_t)y, (uint32_t)(w - 20), (uint32_t)h);
    gfx_text((uint32_t)(x + 12), (uint32_t)(y + 12), e->name, TH_TEXT, GFX_TRANSPARENT);
    gfx_clip_reset();

    size_str(e->size, buf, sizeof(buf));
    gfx_text((uint32_t)(x + 12), (uint32_t)(y + 32), buf, TH_TEXT_MUTED, GFX_TRANSPARENT);
    gfx_hline((uint32_t)(x + 12), (uint32_t)(y + 54), (uint32_t)(w - 24), TH_PANEL_EDGE);

    if (f->preview_len == 0){
        gfx_text((uint32_t)(x + 12), (uint32_t)(y + 66), "(empty)",
                 TH_TEXT_DIM, GFX_TRANSPARENT);
        return;
    }

    // Wrapped as text. Anything unprintable becomes a dot rather than a
    // random glyph, which keeps a binary file from turning the pane to noise.
    int cols = (w - 24) / FONT_W;
    int rows = (h - 76) / FONT_H;
    int col = 0, row = 0;
    for (uint32_t i = 0; i < f->preview_len && row < rows; i++){
        char c = (char)f->preview[i];
        if (c == '\n'){ col = 0; row++; continue; }
        if (c == '\r') continue;
        if (c == '\t'){ col = (col + 4) & ~3; if (col >= cols){ col = 0; row++; } continue; }
        if (c < 32 || c > 126) c = '.';
        gfx_char16((uint32_t)(x + 12 + col * FONT_W), (uint32_t)(y + 66 + row * FONT_H),
                   c, TH_TEXT_MUTED, GFX_TRANSPARENT);
        if (++col >= cols){ col = 0; row++; }
    }
}

static void paint_status(files_t* f, int x, int y, int w){
    gfx_fill_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, STATUS_H, TH_PANEL);
    gfx_hline((uint32_t)x, (uint32_t)y, (uint32_t)w, TH_PANEL_EDGE);

    char buf[64];
    ksnprintf(buf, sizeof(buf), "%d items", f->count);
    gfx_text((uint32_t)(x + 12), (uint32_t)(y + 4), buf, TH_TEXT_MUTED, GFX_TRANSPARENT);

    if (f->sel >= 0 && f->sel < f->count){
        const fat32_dirent_t* e = &f->entries[f->sel];
        if (e->is_dir) ksnprintf(buf, sizeof(buf), "1 item selected");
        else {
            char sz[24];
            size_str(e->size, sz, sizeof(sz));
            ksnprintf(buf, sizeof(buf), "1 item selected   %s", sz);
        }
        gfx_text((uint32_t)(x + 110), (uint32_t)(y + 4), buf, TH_TEXT_MUTED, GFX_TRANSPARENT);
    }
}

// True when the preview pane is showing, which changes how wide the list is
// and therefore where a click lands. One function so paint and hit-testing
// can never disagree about it.
static int preview_open(const files_t* f, int w){
    return (f->sel >= 0 && f->sel < f->count && !f->entries[f->sel].is_dir
            && w > NAV_W + PREVIEW_W + 260);
}

static void paint(wm_window_t* win, files_t* f){
    int x, y, w, h;
    wm_client_rect(win, &x, &y, &w, &h);

    paint_toolbar(f, x, y, w);

    int body_y = y + TOOLBAR_H;
    int body_h = h - TOOLBAR_H - STATUS_H;

    paint_nav(f, x, body_y, NAV_W, body_h);

    int list_x = x + NAV_W;
    int list_w = w - NAV_W;
    if (preview_open(f, w)) list_w -= PREVIEW_W;

    paint_header(list_x, body_y, list_w);
    paint_list(f, list_x, body_y + HEADER_H, list_w, body_h - HEADER_H);

    if (preview_open(f, w))
        paint_preview(f, list_x + list_w, body_y, PREVIEW_W, body_h);

    paint_status(f, x, y + h - STATUS_H, w);
}

// ----------------------------------------------------------------- input

static void on_click(files_t* f, int cx, int cy, int w, int h){
    // Toolbar
    if (cy < TOOLBAR_H){
        if (cy >= 9 && cy < 31){
            if (cx >= 10 && cx < 36){ go_back(f); return; }
            if (cx >= 62 && cx < 88){ go_up(f);   return; }
        }
        return;
    }
    if (cy >= h - STATUS_H) return;

    int body_y = TOOLBAR_H;

    // Navigation pane
    if (cx < NAV_W){
        int ry = cy - (body_y + 32);
        if (ry < 0) return;
        if (ry < ROW_H){ go_root(f); return; }
        int i = ry / (ROW_H + 2) - 1;
        if (i >= 0 && i < f->nav_n){
            // Jumping to a top-level folder from the sidebar lands one level
            // deep whatever the current depth is, so the breadcrumb has to be
            // reset before descending. go_into() records the Back entry.
            f->depth = 0;
            go_into(f, &f->nav[i]);
        }
        return;
    }

    int list_x = NAV_W;
    int list_w = w - NAV_W;
    if (preview_open(f, w)) list_w -= PREVIEW_W;
    if (cx >= list_x + list_w) return;                 // preview pane: inert

    int ry = cy - (body_y + HEADER_H);
    if (ry < 0) return;                                // column header

    int row = ry / ROW_H + f->scroll;
    if (row < 0 || row >= f->count) return;

    // Single click selects, double click opens -- the behaviour of the file
    // manager this is shaped after. The previous version opened on the second
    // click whenever it landed, which meant a folder opened by accident every
    // time somebody clicked the row they had already selected.
    int dbl = (row == f->click_row && ticks - f->click_when < DCLICK_TICKS);
    f->click_row  = row;
    f->click_when = ticks;

    if (f->sel != row){ f->sel = row; load_preview(f); }
    if (dbl){ enter_selected(f); f->click_row = -1; }
}

static void handler(wm_window_t* win, const wm_event_t* ev){
    files_t* f = (files_t*)win->user;
    if (!f) return;

    switch (ev->type){
        case WM_EV_PAINT:
            paint(win, f);
            break;

        case WM_EV_MOUSE_DOWN: {
            int x, y, w, h;
            wm_client_rect(win, &x, &y, &w, &h);
            on_click(f, ev->x, ev->y, w, h);
            wm_invalidate();
        } break;

        case WM_EV_KEY:
            if      (ev->key == KEY_UP   && f->sel > 0)            { f->sel--; load_preview(f); }
            else if (ev->key == KEY_DOWN && f->sel < f->count - 1) { f->sel++; load_preview(f); }
            else if (ev->key == '\n')                              enter_selected(f);
            else if (ev->key == '\b')                              go_up(f);
            else if (ev->key == KEY_LEFT)                          go_back(f);
            else if (ev->key == KEY_RIGHT)                         enter_selected(f);
            wm_invalidate();
            break;

        case WM_EV_CLOSE:
            if (f->preview) kfree(f->preview);
            kfree(f);
            win->user = 0;
            break;

        default: break;
    }
}

void app_files_open(void){
    files_t* f = (files_t*)kmalloc(sizeof(files_t));
    if (!f) return;
    memset(f, 0, sizeof(*f));
    f->sel = -1;
    f->click_row = -1;

    f->preview = (uint8_t*)kmalloc(PREVIEW_CAP);

    shell_init();                       // idempotent; mounts the volume once
    f->mounted = shell_fs_ready();
    if (f->mounted){
        load_nav(f);
        load_dir(f, fat32_root_cluster());
    }

    wm_open("Files", 120, 90, 820, 480, handler, f);
}
