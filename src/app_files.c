// src/app_files.c — File Browser
//
// A two-pane browser over the FAT32 volume: directory listing on the left,
// a preview of the selected file on the right. Read-only, because the FAT32
// driver is read-only — nothing here can corrupt the volume.
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

#define MAX_ENTRIES  128
#define PREVIEW_CAP  (8u * 1024u)
#define ROW_H        18
#define LIST_W       260

// Deep enough for any directory tree this filesystem is likely to hold, and
// a fixed array means navigation can never fail on an allocation.
#define DEPTH_MAX 12

typedef struct {
    uint32_t       cluster;
    fat32_dirent_t entries[MAX_ENTRIES];
    int            count;
    int            sel;
    int            scroll;

    uint32_t       stack[DEPTH_MAX];      // ancestor clusters, for going up
    int            depth;

    uint8_t*       preview;
    uint32_t       preview_len;
    int            preview_is_dir;
    char           preview_name[FAT32_NAME_MAX];
    int            mounted;
} files_t;

static int is_dot(const char* n){ return n[0] == '.' && n[1] == 0; }
static int is_dotdot(const char* n){ return n[0] == '.' && n[1] == '.' && n[2] == 0; }

static void load_preview(files_t* f);

static void load_dir(files_t* f, uint32_t cluster){
    f->cluster = cluster;
    f->count   = fat32_list(cluster, f->entries, MAX_ENTRIES);
    if (f->count < 0) f->count = 0;
    f->sel     = 0;
    f->scroll  = 0;
    load_preview(f);
}

static void load_preview(files_t* f){
    f->preview_len    = 0;
    f->preview_is_dir = 0;
    f->preview_name[0] = 0;
    if (f->sel < 0 || f->sel >= f->count || !f->preview) return;

    const fat32_dirent_t* e = &f->entries[f->sel];
    strncpy(f->preview_name, e->name, FAT32_NAME_MAX - 1);
    f->preview_name[FAT32_NAME_MAX - 1] = 0;

    if (e->is_dir){ f->preview_is_dir = 1; return; }

    uint32_t cap = e->size < PREVIEW_CAP ? e->size : PREVIEW_CAP;
    f->preview_len = fat32_read_file(e, f->preview, cap);
}

static void enter_selected(files_t* f){
    if (f->sel < 0 || f->sel >= f->count) return;
    const fat32_dirent_t* e = &f->entries[f->sel];
    if (!e->is_dir) return;

    if (is_dot(e->name)) return;

    if (is_dotdot(e->name)){
        if (f->depth > 0) load_dir(f, f->stack[--f->depth]);
        return;
    }

    if (f->depth < DEPTH_MAX){
        f->stack[f->depth++] = f->cluster;
        // A directory entry whose first cluster is 0 means the root; FAT32
        // stores the root's parent that way in every ".." entry.
        load_dir(f, e->first_cluster ? e->first_cluster : fat32_root_cluster());
    }
}

static void go_up(files_t* f){
    if (f->depth > 0) load_dir(f, f->stack[--f->depth]);
}

static void paint_list(files_t* f, int x, int y, int w, int h){
    gfx_fill_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, TH_FIELD);
    gfx_vline((uint32_t)(x + w - 1), (uint32_t)y, (uint32_t)h, TH_PANEL_EDGE);

    if (!f->mounted){
        gfx_text((uint32_t)(x + 12), (uint32_t)(y + 14), "no FAT32 volume", WM_COL_TEXT_MUTED, GFX_TRANSPARENT);
        gfx_text((uint32_t)(x + 12), (uint32_t)(y + 32), "mounted on the ATA disk", WM_COL_TEXT_MUTED, GFX_TRANSPARENT);
        return;
    }

    int visible = h / ROW_H;
    if (f->sel < f->scroll) f->scroll = f->sel;
    if (f->sel >= f->scroll + visible) f->scroll = f->sel - visible + 1;

    for (int i = 0; i < visible && f->scroll + i < f->count; i++){
        int idx = f->scroll + i;
        int ry  = y + i * ROW_H;
        const fat32_dirent_t* e = &f->entries[idx];

        if (idx == f->sel)
            gfx_fill_rect((uint32_t)x, (uint32_t)ry, (uint32_t)(w - 1), ROW_H, WM_COL_SEL);

        // Folder / file pictogram, so directories read at a glance.
        if (e->is_dir){
            gfx_fill_rect((uint32_t)(x + 8), (uint32_t)(ry + 5), 5, 3, WM_COL_ACCENT);
            gfx_fill_rect((uint32_t)(x + 8), (uint32_t)(ry + 7), 11, 7, WM_COL_ACCENT);
        } else {
            gfx_fill_rect((uint32_t)(x + 9), (uint32_t)(ry + 4), 9, 11, TH_TEXT_DIM);
            gfx_fill_rect((uint32_t)(x + 11), (uint32_t)(ry + 6), 5, 1, TH_FIELD);
            gfx_fill_rect((uint32_t)(x + 11), (uint32_t)(ry + 9), 5, 1, TH_FIELD);
        }

        gfx_text((uint32_t)(x + 26), (uint32_t)(ry + 1), e->name, WM_COL_TEXT_DARK, GFX_TRANSPARENT);
    }
}

static void paint_preview(files_t* f, int x, int y, int w, int h){
    char line[80];

    if (!f->preview_name[0]){
        gfx_text((uint32_t)(x + 12), (uint32_t)(y + 12), "select a file", WM_COL_TEXT_MUTED, GFX_TRANSPARENT);
        return;
    }

    gfx_text((uint32_t)(x + 12), (uint32_t)(y + 8), f->preview_name, WM_COL_TEXT_DARK, GFX_TRANSPARENT);

    if (f->preview_is_dir){
        gfx_text((uint32_t)(x + 12), (uint32_t)(y + 30), "directory - press Enter to open",
                 WM_COL_TEXT_MUTED, GFX_TRANSPARENT);
        return;
    }

    ksnprintf(line, sizeof(line), "%u bytes", f->entries[f->sel].size);
    gfx_text((uint32_t)(x + 12), (uint32_t)(y + 26), line, WM_COL_TEXT_MUTED, GFX_TRANSPARENT);
    gfx_hline((uint32_t)(x + 12), (uint32_t)(y + 46), (uint32_t)(w - 24), TH_PANEL_EDGE);

    // Wrap the bytes into the pane as text. Anything unprintable becomes a
    // dot rather than a random glyph, which keeps a binary file from turning
    // the pane into noise.
    int cols = (w - 24) / FONT_W;
    int rows = (h - 60) / FONT_H;
    int col = 0, row = 0;

    for (uint32_t i = 0; i < f->preview_len && row < rows; i++){
        char c = (char)f->preview[i];
        if (c == '\n'){ col = 0; row++; continue; }
        if (c == '\r') continue;
        if (c == '\t'){ col = (col + 4) & ~3; if (col >= cols){ col = 0; row++; } continue; }
        if (c < 32 || c > 126) c = '.';

        gfx_char16((uint32_t)(x + 12 + col * FONT_W), (uint32_t)(y + 54 + row * FONT_H),
                   c, WM_COL_TEXT_DARK, GFX_TRANSPARENT);
        if (++col >= cols){ col = 0; row++; }
    }

    if (f->preview_len == 0)
        gfx_text((uint32_t)(x + 12), (uint32_t)(y + 54), "(empty)", WM_COL_TEXT_MUTED, GFX_TRANSPARENT);
}

static void paint(wm_window_t* win, files_t* f){
    int x, y, w, h;
    wm_client_rect(win, &x, &y, &w, &h);

    char bar[64];
    ksnprintf(bar, sizeof(bar), "cluster %u   %d items   depth %d", f->cluster, f->count, f->depth);
    gfx_fill_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, 24, TH_PANEL);
    gfx_text((uint32_t)(x + 10), (uint32_t)(y + 4), bar, WM_COL_TEXT_MUTED, GFX_TRANSPARENT);
    gfx_hline((uint32_t)x, (uint32_t)(y + 23), (uint32_t)w, TH_PANEL_EDGE);

    paint_list(f, x, y + 24, LIST_W, h - 24 - 22);
    paint_preview(f, x + LIST_W, y + 24, w - LIST_W, h - 24 - 22);

    int fy = y + h - 20;
    gfx_hline((uint32_t)x, (uint32_t)(fy - 2), (uint32_t)w, TH_PANEL_EDGE);
    gfx_text((uint32_t)(x + 10), (uint32_t)(fy + 1),
             "up/down move   Enter open   Backspace up", WM_COL_TEXT_MUTED, GFX_TRANSPARENT);
}

static void handler(wm_window_t* win, const wm_event_t* ev){
    files_t* f = (files_t*)win->user;
    if (!f) return;

    switch (ev->type){
        case WM_EV_PAINT:
            paint(win, f);
            break;

        case WM_EV_MOUSE_DOWN: {
            if (ev->x >= LIST_W || ev->y < 24) break;
            int row = (ev->y - 24) / ROW_H + f->scroll;
            if (row >= 0 && row < f->count){
                if (row == f->sel) enter_selected(f);      // second click opens
                else { f->sel = row; load_preview(f); }
                wm_invalidate();
            }
        } break;

        case WM_EV_KEY:
            if      (ev->key == KEY_UP   && f->sel > 0)            { f->sel--; load_preview(f); }
            else if (ev->key == KEY_DOWN && f->sel < f->count - 1) { f->sel++; load_preview(f); }
            else if (ev->key == '\n')                              enter_selected(f);
            else if (ev->key == '\b')                              go_up(f);
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

    f->preview = (uint8_t*)kmalloc(PREVIEW_CAP);

    shell_init();                       // idempotent; mounts the volume once
    f->mounted = shell_fs_ready();
    if (f->mounted) load_dir(f, fat32_root_cluster());

    wm_open("Files", 120, 90, 700, 440, handler, f);
}
