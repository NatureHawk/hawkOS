// src/app_files.c — Files
//
// Laid out the way Windows Explorer is, because that is the arrangement most
// people already know how to use: a navigation pane down the left, a details
// list in the middle with a column header, a breadcrumb address bar and
// back/forward/up buttons across the top, and a status line at the bottom.
// A preview pane appears on the right when a file is selected and gets out of
// the way when one is not.
//
// Under the address bar is a command bar -- New folder, New file, Cut, Copy,
// Paste, Rename, Delete -- and the same actions are on the keyboard (Ctrl+C,
// Ctrl+X, Ctrl+V, Delete, F2, Ctrl+N, Ctrl+Shift+N). Selection is a set, not
// a single row: Ctrl+click toggles, Shift+click and Shift+arrows extend,
// Ctrl+A takes everything.
//
// What the operations do to the disk lives in fsutil.c; this file decides what
// the user asked for and tells them what happened. Names that need typing go
// through a small modal dialog drawn over the list, because a rename that
// happens in some other window would be a rename nobody could see.
#include <stdint.h>
#include "header/apps.h"
#include "header/wm.h"
#include "header/theme.h"
#include "header/gfx.h"
#include "header/fat32.h"
#include "header/fsutil.h"
#include "header/clipboard.h"
#include "header/lineedit.h"
#include "header/kheap.h"
#include "header/kstring.h"
#include "header/kbd.h"
#include "header/mouse.h"
#include "header/shell.h"
#include "header/font.h"

extern volatile unsigned long long ticks;

#define MAX_ENTRIES  128
#define PREVIEW_CAP  (8u * 1024u)

#define ROW_H        20
#define NAV_W        158
#define PREVIEW_W    230
#define TOOLBAR_H    40
#define CMD_H        38
#define HEADER_H     24
#define STATUS_H     24

#define COL_TYPE     280        // from the left edge of the list pane
#define COL_SIZE     420

#define TOP_H        (TOOLBAR_H + CMD_H)

// Deep enough for any tree this filesystem is likely to hold, and a fixed
// array means navigation can never fail on an allocation.
#define DEPTH_MAX 12
#define NAV_MAX   10

// The PIT runs at 100 Hz. Windows' own default double-click time is 500 ms.
#define DCLICK_TICKS 50
#define FLASH_TICKS  400

enum { FM_NONE = 0, FM_NEWDIR, FM_NEWFILE, FM_RENAME, FM_DELETE };
enum { C_NEWDIR, C_NEWFILE, C_CUT, C_COPY, C_PASTE, C_RENAME, C_DELETE, C_N };

typedef struct {
    uint32_t       cluster;
    fat32_dirent_t entries[MAX_ENTRIES];
    uint8_t        marked[MAX_ENTRIES];     // the selection
    int            count;
    int            sel;                     // the row with focus
    int            anchor;                  // where a Shift-extension started
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

    // The dialog. `mode` says which one is up; while it is, the rest of the
    // window is inert.
    int            mode;
    lineedit_t     input;
    int            input_drag;
    char           dlg_err[96];
    char           target[FAT32_NAME_MAX];     // the name being renamed

    // One line of feedback for the status bar, and how long it stays.
    char           flash[112];
    int            flash_err;
    unsigned long long flash_when;

    uint32_t       free_kb;
} files_t;

// Names handed to the clipboard. File scope rather than on the stack: 64 names
// of 128 bytes is 8 KB, more than a window handler should take from a task
// stack it shares with everything else.
static char clip_names[CLIP_FILES_MAX][CLIP_NAME_MAX];

static int is_dot(const char* n){ return n[0] == '.' && n[1] == 0; }
static int is_dotdot(const char* n){ return n[0] == '.' && n[1] == '.' && n[2] == 0; }

typedef struct { int x, y, w, h; } rect_t;
static int inside(rect_t r, int x, int y){
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static void load_preview(files_t* f);

static void say(files_t* f, int err, const char* s){
    strncpy(f->flash, s, sizeof(f->flash) - 1);
    f->flash[sizeof(f->flash) - 1] = 0;
    f->flash_err = err;
    f->flash_when = ticks;
}

// ------------------------------------------------------------- selection

static int sel_count(const files_t* f){
    int n = 0;
    for (int i = 0; i < f->count; i++) if (f->marked[i]) n++;
    return n;
}

// The one selected entry, or null when there are none or several.
static const fat32_dirent_t* sole(const files_t* f){
    int found = -1;
    for (int i = 0; i < f->count; i++)
        if (f->marked[i]){ if (found >= 0) return 0; found = i; }
    return found >= 0 ? &f->entries[found] : 0;
}

static void select_only(files_t* f, int i){
    memset(f->marked, 0, sizeof(f->marked));
    if (i >= 0 && i < f->count) f->marked[i] = 1;
    f->sel = f->anchor = (i >= 0 && i < f->count) ? i : -1;
    load_preview(f);
}

static void select_range(files_t* f, int a, int b){
    if (a > b){ int t = a; a = b; b = t; }
    memset(f->marked, 0, sizeof(f->marked));
    for (int i = a; i <= b && i < f->count; i++) if (i >= 0) f->marked[i] = 1;
    load_preview(f);
}

static void select_all(files_t* f){
    for (int i = 0; i < f->count; i++) f->marked[i] = 1;
    if (f->sel < 0 && f->count) f->sel = 0;
    load_preview(f);
}

static void keep_row_visible(files_t* f, int visible){
    if (f->sel < 0 || visible < 1) return;
    if (f->sel < f->scroll) f->scroll = f->sel;
    if (f->sel >= f->scroll + visible) f->scroll = f->sel - visible + 1;
}

static int visible_rows(int client_h){
    int v = (client_h - TOP_H - HEADER_H - STATUS_H) / ROW_H;
    return v < 1 ? 1 : v;
}

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

static void refresh_free(files_t* f){
    // 64-bit: a large disk's free bytes do not fit in 32, and this disk's 512-byte
    // clusters make "bytes per cluster / 1024" come out as zero.
    f->free_kb = (uint32_t)(((uint64_t)fat32_free_clusters() * fat32_bytes_per_cluster()) >> 10);
}

static void load_dir(files_t* f, uint32_t cluster){
    // Long names make an entry ~150 bytes, so the scratch listing lives on the
    // heap rather than the (task) stack.
    fat32_dirent_t* raw = (fat32_dirent_t*)kmalloc(sizeof(fat32_dirent_t) * MAX_ENTRIES);
    if (!raw) return;
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

    kfree(raw);
    f->cluster = cluster;
    f->scroll  = 0;
    sort_entries(f);
    select_only(f, f->count ? 0 : -1);
    refresh_free(f);
}

static void load_nav(files_t* f){
    fat32_dirent_t* raw = (fat32_dirent_t*)kmalloc(sizeof(fat32_dirent_t) * MAX_ENTRIES);
    f->nav_n = 0;
    if (!raw) return;
    int n = fat32_list(fat32_root_cluster(), raw, MAX_ENTRIES);
    for (int i = 0; i < n && f->nav_n < NAV_MAX; i++){
        if (!raw[i].is_dir || is_dot(raw[i].name) || is_dotdot(raw[i].name)) continue;
        f->nav[f->nav_n++] = raw[i];
    }
    kfree(raw);
}

// Reads the folder again after something changed it, putting the selection on
// `select_name` when there is one -- the folder just made, the file just
// renamed -- and keeping the scroll position otherwise.
static void reload(files_t* f, const char* select_name){
    int scroll = f->scroll;
    load_dir(f, f->cluster);
    load_nav(f);
    f->scroll = scroll;
    if (select_name){
        for (int i = 0; i < f->count; i++)
            if (kstricmp(f->entries[i].name, select_name) == 0){
                select_only(f, i);
                if (i < f->scroll || i >= f->scroll + 8) f->scroll = i > 3 ? i - 3 : 0;
                break;
            }
    }
    if (f->scroll >= f->count) f->scroll = f->count ? f->count - 1 : 0;
}

static void load_preview(files_t* f){
    f->preview_len    = 0;
    f->preview_is_dir = 0;
    const fat32_dirent_t* e = sole(f);
    if (!e || !f->preview) return;
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

// Opens the focused entry: a folder is entered, a text file goes to the
// editor. Anything else has no program to open it, and the status bar says so
// instead of nothing happening.
static void open_entry(files_t* f, const fat32_dirent_t* e){
    if (e->is_dir){ go_into(f, e); return; }

    int text = fs_is_text_name(e->name);
    if (!text){
        uint8_t head[512];
        uint32_t n = e->size < sizeof(head) ? e->size : (uint32_t)sizeof(head);
        uint32_t got = n ? fat32_read_file(e, head, n) : 0;
        text = fs_looks_like_text(head, got);
    }
    if (!text){ say(f, 1, "No app is set up to open this kind of file"); return; }

    int r = app_edit_open_file(f->cluster, e);
    if (r == -1)      say(f, 1, "That file is too large for the editor (128 KB limit)");
    else if (r == -2) say(f, 1, "That file does not look like text");
    else if (r < 0)   say(f, 1, "Could not read that file");
}

static void open_selected(files_t* f){
    const fat32_dirent_t* e = sole(f);
    if (e) open_entry(f, e);
}

// ------------------------------------------------------------- operations

static int writable(files_t* f){
    if (!f->mounted){ say(f, 1, "There is no volume mounted"); return 0; }
    if (!fat32_writable()){ say(f, 1, "The disk is read-only"); return 0; }
    return 1;
}

static void do_copy(files_t* f, int cut){
    int n = 0;
    for (int i = 0; i < f->count && n < CLIP_FILES_MAX; i++)
        if (f->marked[i]){
            strncpy(clip_names[n], f->entries[i].name, CLIP_NAME_MAX - 1);
            clip_names[n][CLIP_NAME_MAX - 1] = 0;
            n++;
        }
    if (!n) return;
    clip_set_files(f->cluster, (const char (*)[CLIP_NAME_MAX])clip_names, n, cut);

    char m[64];
    ksnprintf(m, sizeof(m), "%s %d item%s", cut ? "Cut" : "Copied", n, n == 1 ? "" : "s");
    say(f, 0, m);
}

static void do_paste(files_t* f){
    int n = clip_file_count();
    if (!n){ say(f, 1, "There is nothing to paste"); return; }
    if (!writable(f)) return;

    uint32_t src = clip_files_dir(), dst = f->cluster;
    int cut = clip_files_cut();
    int ok = 0, bad = 0, loop = 0;
    char last[FAT32_NAME_MAX];
    last[0] = 0;

    for (int i = 0; i < n; i++){
        fat32_dirent_t e;
        if (fat32_find(src, clip_file_name(i), &e) != 0){ bad++; continue; }

        char nn[FAT32_NAME_MAX];
        if (cut){
            if (src == dst){ ok++; continue; }          // already here
            if (e.is_dir && fs_dir_inside(e.first_cluster, dst)){ loop++; continue; }
            fs_unique_name(dst, e.name, nn, sizeof(nn));
            if (fat32_rename(src, e.name, dst, nn) == 0){ ok++; strncpy(last, nn, sizeof(last) - 1); }
            else bad++;
        } else {
            fs_unique_name(dst, e.name, nn, sizeof(nn));
            if (fs_copy(src, &e, dst, nn) == 0){ ok++; strncpy(last, nn, sizeof(last) - 1); }
            else bad++;
        }
        last[sizeof(last) - 1] = 0;
    }

    // A move is spent once it has happened; a copy can be pasted again.
    if (cut && ok && !bad && !loop) clip_files_clear();

    reload(f, last[0] ? last : 0);

    char m[112];
    if (loop)      say(f, 1, "A folder cannot be moved into itself");
    else if (bad){ ksnprintf(m, sizeof(m), "%d item%s could not be %s", bad, bad == 1 ? "" : "s",
                             cut ? "moved" : "copied"); say(f, 1, m); }
    else           { ksnprintf(m, sizeof(m), "%s %d item%s", cut ? "Moved" : "Pasted", ok,
                               ok == 1 ? "" : "s"); say(f, 0, m); }
}

static void do_delete(files_t* f){
    if (!writable(f)) return;
    int ok = 0, bad = 0;
    for (int i = 0; i < f->count; i++){
        if (!f->marked[i]) continue;
        if (fs_delete(f->cluster, &f->entries[i]) == 0) ok++; else bad++;
    }
    int keep = f->sel;
    reload(f, 0);
    if (f->count) select_only(f, keep < f->count ? keep : f->count - 1);

    char m[112];
    if (bad){ ksnprintf(m, sizeof(m), "%d item%s could not be deleted", bad, bad == 1 ? "" : "s"); say(f, 1, m); }
    else    { ksnprintf(m, sizeof(m), "Deleted %d item%s", ok, ok == 1 ? "" : "s"); say(f, 0, m); }
}

// --------------------------------------------------------------- dialogs

static void unique_default(files_t* f, const char* base, const char* ext, char* out, uint32_t cap){
    ksnprintf(out, cap, "%s%s", base, ext);
    for (int n = 2; n < 100 && fs_exists(f->cluster, out, 0); n++)
        ksnprintf(out, cap, "%s (%d)%s", base, n, ext);
}

// Selects the part of a name before its extension, which is the part someone
// renaming a file wants to type over.
static void select_stem(lineedit_t* le){
    int dot = -1;
    for (int i = 0; i < le->len; i++) if (le->text[i] == '.' && i > 0) dot = i;
    le->anc = 0;
    le->cur = dot > 0 ? dot : le->len;
}

static void open_dialog(files_t* f, int mode){
    if (!f->mounted){ say(f, 1, "There is no volume mounted"); return; }
    f->dlg_err[0] = 0;
    f->input_drag = 0;

    if (mode == FM_NEWDIR || mode == FM_NEWFILE){
        if (!writable(f)) return;
        char nm[FAT32_NAME_MAX];
        if (mode == FM_NEWDIR) unique_default(f, "New folder", "", nm, sizeof(nm));
        else                   unique_default(f, "New Text Document", ".txt", nm, sizeof(nm));
        le_set(&f->input, nm, 0);
        select_stem(&f->input);
    } else if (mode == FM_RENAME){
        const fat32_dirent_t* e = sole(f);
        if (!e) return;
        if (!writable(f)) return;
        strncpy(f->target, e->name, sizeof(f->target) - 1);
        f->target[sizeof(f->target) - 1] = 0;
        le_set(&f->input, e->name, 0);
        if (!e->is_dir) select_stem(&f->input); else le_set(&f->input, e->name, 1);
    } else if (mode == FM_DELETE){
        if (!sel_count(f)) return;
        if (!writable(f)) return;
    }
    f->mode = mode;
}

static void commit_dialog(files_t* f){
    const char* nm = f->input.text;

    if (f->mode == FM_DELETE){
        f->mode = FM_NONE;
        do_delete(f);
        return;
    }

    if (!fs_name_ok(nm)){
        strncpy(f->dlg_err, "That name cannot be used (no \\ / : * ? \" < > |)", sizeof(f->dlg_err) - 1);
        return;
    }

    if (f->mode == FM_RENAME){
        if (strcmp(nm, f->target) == 0){ f->mode = FM_NONE; return; }
        // A change of case alone is a rename, though the two names compare equal.
        if (kstricmp(nm, f->target) != 0 && fs_exists(f->cluster, nm, 0)){
            strncpy(f->dlg_err, "Something with that name already exists here",
                    sizeof(f->dlg_err) - 1);
            return;
        }
        if (fat32_rename(f->cluster, f->target, f->cluster, nm) != 0){
            strncpy(f->dlg_err, "The name could not be changed", sizeof(f->dlg_err) - 1);
            return;
        }
        f->mode = FM_NONE;
        char made[FAT32_NAME_MAX];
        strncpy(made, nm, sizeof(made) - 1); made[sizeof(made) - 1] = 0;
        reload(f, made);
        say(f, 0, "Renamed");
        return;
    }

    if (fs_exists(f->cluster, nm, 0)){
        strncpy(f->dlg_err, "Something with that name already exists here", sizeof(f->dlg_err) - 1);
        return;
    }
    int r = (f->mode == FM_NEWDIR)
          ? fat32_mkdir(f->cluster, nm)
          : fat32_write_file(f->cluster, nm, (const uint8_t*)"", 0);
    if (r != 0){
        strncpy(f->dlg_err, "Could not create it: the disk may be full", sizeof(f->dlg_err) - 1);
        return;
    }
    char made[FAT32_NAME_MAX];
    strncpy(made, nm, sizeof(made) - 1); made[sizeof(made) - 1] = 0;
    int was_dir = (f->mode == FM_NEWDIR);
    f->mode = FM_NONE;
    reload(f, made);
    say(f, 0, was_dir ? "Folder created" : "File created - double-click it to edit");
}

typedef struct { rect_t card, field, ok, cancel; } dlg_t;

static dlg_t dlg_geom(const files_t* f, int w){
    dlg_t d;
    int dw = 400, dh = (f->mode == FM_DELETE) ? 150 : 178;
    if (dw > w - 24) dw = w - 24;
    d.card   = (rect_t){ (w - dw) / 2, TOP_H + 26, dw, dh };
    d.field  = (rect_t){ d.card.x + 16, d.card.y + 72, dw - 32, 30 };
    int by   = d.card.y + dh - 16 - 30;
    d.cancel = (rect_t){ d.card.x + dw - 16 - 92, by, 92, 30 };
    d.ok     = (rect_t){ d.cancel.x - 8 - 92, by, 92, 30 };
    return d;
}

// -------------------------------------------------------- command bar

static int cmd_labelled(int w){ return w >= 720; }

static rect_t cmd_rect(int i, int w){
    static const char* const L[C_N] = { "New folder", "New file", "Cut", "Copy", "Paste",
                                        "Rename", "Delete" };
    int x = 10;
    for (int k = 0; k <= i; k++){
        if (k == C_CUT || k == C_RENAME) x += 14;
        int bw = cmd_labelled(w) ? 16 + 8 + (int)gfx_text_width(L[k]) + 20 : 36;
        if (k == i) return (rect_t){ x, TOOLBAR_H + 4, bw, 30 };
        x += bw + 2;
    }
    return (rect_t){ 0, 0, 0, 0 };
}

static int cmd_enabled(const files_t* f, int id){
    if (!f->mounted) return 0;
    int n = sel_count(f);
    switch (id){
        case C_CUT: case C_COPY: case C_DELETE: return n > 0;
        case C_RENAME: return n == 1;
        case C_PASTE:  return clip_file_count() > 0;
        default:       return 1;
    }
}

static void run_cmd(files_t* f, int id){
    switch (id){
        case C_NEWDIR:  open_dialog(f, FM_NEWDIR);  break;
        case C_NEWFILE: open_dialog(f, FM_NEWFILE); break;
        case C_CUT:     do_copy(f, 1); break;
        case C_COPY:    do_copy(f, 0); break;
        case C_PASTE:   do_paste(f);   break;
        case C_RENAME:  open_dialog(f, FM_RENAME);  break;
        case C_DELETE:  open_dialog(f, FM_DELETE);  break;
        default: break;
    }
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

static void plus(int x, int y, uint32_t c){
    gfx_fill_rect((uint32_t)(x + 2), (uint32_t)y, 2, 7, c);
    gfx_fill_rect((uint32_t)x, (uint32_t)(y + 2), 7, 2, c);
}

// The command bar's pictograms, each in a 16x16 cell at (x, y).
static void cmd_icon(int id, int x, int y, uint32_t c){
    switch (id){
        case C_NEWDIR:
            icon_folder(x, y + 1);
            plus(x + 9, y + 8, TH_OK);
            break;
        case C_NEWFILE:
            icon_file(x, y);
            plus(x + 9, y + 8, TH_OK);
            break;
        case C_CUT:
            for (int i = 0; i < 10; i++){
                gfx_put_pixel((uint32_t)(x + 3 + i), (uint32_t)(y + 1 + i), c);
                gfx_put_pixel((uint32_t)(x + 12 - i), (uint32_t)(y + 1 + i), c);
            }
            gfx_draw_circle(x + 4, y + 12, 2, c);
            gfx_draw_circle(x + 11, y + 12, 2, c);
            break;
        case C_COPY:
            gfx_draw_rect((uint32_t)(x + 1), (uint32_t)(y + 1), 9, 11, c);
            gfx_fill_rect((uint32_t)(x + 5), (uint32_t)(y + 4), 9, 11, TH_PANEL);
            gfx_draw_rect((uint32_t)(x + 5), (uint32_t)(y + 4), 9, 11, c);
            break;
        case C_PASTE:
            gfx_draw_rect((uint32_t)(x + 2), (uint32_t)(y + 2), 12, 13, c);
            gfx_fill_rect((uint32_t)(x + 5), (uint32_t)y, 6, 4, c);
            gfx_hline((uint32_t)(x + 5), (uint32_t)(y + 7), 6, c);
            gfx_hline((uint32_t)(x + 5), (uint32_t)(y + 10), 6, c);
            break;
        case C_RENAME:
            gfx_draw_rect((uint32_t)x, (uint32_t)(y + 3), 15, 10, c);
            gfx_fill_rect((uint32_t)(x + 5), (uint32_t)(y + 5), 2, 6, c);
            break;
        case C_DELETE:
            gfx_hline((uint32_t)(x + 2), (uint32_t)(y + 2), 12, c);
            gfx_fill_rect((uint32_t)(x + 6), (uint32_t)y, 4, 2, c);
            gfx_draw_rect((uint32_t)(x + 3), (uint32_t)(y + 4), 10, 11, c);
            gfx_vline((uint32_t)(x + 6), (uint32_t)(y + 6), 7, c);
            gfx_vline((uint32_t)(x + 9), (uint32_t)(y + 6), 7, c);
            break;
        default: break;
    }
}

// ---------------------------------------------------------------- painting

static void paint_toolbar(files_t* f, int x, int y, int w){
    gfx_fill_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, TOOLBAR_H, TH_PANEL);

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

static void paint_cmdbar(files_t* f, int x, int y, int w){
    static const char* const L[C_N] = { "New folder", "New file", "Cut", "Copy", "Paste",
                                        "Rename", "Delete" };
    gfx_fill_rect((uint32_t)x, (uint32_t)(y + TOOLBAR_H), (uint32_t)w, CMD_H, TH_PANEL);
    gfx_hline((uint32_t)x, (uint32_t)(y + TOP_H - 1), (uint32_t)w, TH_PANEL_EDGE);

    int mx = mouse_x(), my = mouse_y();
    for (int i = 0; i < C_N; i++){
        rect_t r = cmd_rect(i, w);
        r.x += x; r.y += y;
        int on = cmd_enabled(f, i);
        int hot = on && inside(r, mx, my) && f->mode == FM_NONE;
        if (hot)
            gfx_fill_round_rect((uint32_t)r.x, (uint32_t)r.y, (uint32_t)r.w, (uint32_t)r.h, 6, TH_HOVER);

        uint32_t c = on ? TH_TEXT : TH_TEXT_DIM;
        if (i == C_DELETE && on) c = TH_ERROR;
        int ix = cmd_labelled(w) ? r.x + 10 : r.x + (r.w - 16) / 2;
        cmd_icon(i, ix, r.y + 7, c);
        if (cmd_labelled(w))
            gfx_text((uint32_t)(ix + 24), (uint32_t)(r.y + 7), L[i], c, GFX_TRANSPARENT);
    }
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

// True for an entry in this folder that has been Cut and not yet pasted; it is
// drawn faded, the way a file waiting to be moved is on every other desktop.
static int is_cut_pending(const files_t* f, const char* name){
    if (!clip_files_cut() || clip_files_dir() != f->cluster) return 0;
    for (int i = 0; i < clip_file_count(); i++)
        if (kstricmp(clip_file_name(i), name) == 0) return 1;
    return 0;
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
    int max_scroll = f->count - visible;
    if (max_scroll < 0) max_scroll = 0;
    if (f->scroll > max_scroll) f->scroll = max_scroll;
    if (f->scroll < 0) f->scroll = 0;

    char tbuf[24], sbuf[24];
    for (int i = 0; i < visible && f->scroll + i < f->count; i++){
        int idx = f->scroll + i;
        int ry  = y + i * ROW_H;
        const fat32_dirent_t* e = &f->entries[idx];
        int on = f->marked[idx];

        if (on) gfx_fill_rect((uint32_t)x, (uint32_t)ry, (uint32_t)w, ROW_H, TH_SELECT);
        // The focus row, when it is not selected, gets an outline so keyboard
        // movement across a multi-selection can be followed.
        else if (idx == f->sel && sel_count(f) > 1)
            gfx_draw_rect((uint32_t)x, (uint32_t)ry, (uint32_t)w, ROW_H, TH_SELECT);

        int faded = is_cut_pending(f, e->name);
        uint32_t fg = on ? TH_SELECT_TEXT : (faded ? TH_TEXT_DIM : TH_TEXT);

        if (e->is_dir) icon_folder(x + 10, ry + 3);
        else           icon_file(x + 10, ry + 3);

        // Long file names can outrun the Name column; clip rather than let
        // them print over the Type and Size columns.
        int name_w = (w > COL_TYPE + 60 ? COL_TYPE : w) - 34 - 6;
        if (name_w < 16) name_w = 16;
        gfx_clip_set((uint32_t)(x + 34), (uint32_t)ry, (uint32_t)name_w, ROW_H);
        gfx_text((uint32_t)(x + 34), (uint32_t)(ry + 2), e->name, fg, GFX_TRANSPARENT);
        gfx_clip_reset();

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

    const fat32_dirent_t* e = sole(f);
    if (!e) return;
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

    int n = sel_count(f);
    if (n == 1){
        const fat32_dirent_t* e = sole(f);
        if (e->is_dir) ksnprintf(buf, sizeof(buf), "1 item selected");
        else {
            char sz[24];
            size_str(e->size, sz, sizeof(sz));
            ksnprintf(buf, sizeof(buf), "1 item selected   %s", sz);
        }
        gfx_text((uint32_t)(x + 110), (uint32_t)(y + 4), buf, TH_TEXT_MUTED, GFX_TRANSPARENT);
    } else if (n > 1){
        ksnprintf(buf, sizeof(buf), "%d items selected", n);
        gfx_text((uint32_t)(x + 110), (uint32_t)(y + 4), buf, TH_TEXT_MUTED, GFX_TRANSPARENT);
    }

    // Feedback from the last operation replaces the free-space figure for a
    // few seconds, then gives it back.
    char right[120];
    uint32_t col = TH_TEXT_MUTED;
    if (f->flash[0] && ticks - f->flash_when < FLASH_TICKS){
        strncpy(right, f->flash, sizeof(right) - 1);
        right[sizeof(right) - 1] = 0;
        col = f->flash_err ? TH_ERROR : TH_OK;
    } else if (f->mounted){
        if (f->free_kb >= 1024) ksnprintf(right, sizeof(right), "%u MB free", f->free_kb / 1024u);
        else                    ksnprintf(right, sizeof(right), "%u KB free", f->free_kb);
    } else right[0] = 0;

    int rw = (int)gfx_text_width(right);
    int rx = x + w - 12 - rw;
    if (rx < x + 300) rx = x + 300;
    gfx_clip_set((uint32_t)(x + 300), (uint32_t)y, (uint32_t)(w - 300), STATUS_H);
    gfx_text((uint32_t)rx, (uint32_t)(y + 4), right, col, GFX_TRANSPARENT);
    gfx_clip_reset();
}

static void draw_btn(rect_t r, const char* label, int primary, int danger, int hot){
    uint32_t fill = primary ? (danger ? TH_ERROR : TH_ACCENT) : (hot ? TH_HOVER : TH_CONTROL);
    gfx_fill_round_rect((uint32_t)r.x, (uint32_t)r.y, (uint32_t)r.w, (uint32_t)r.h, 7, fill);
    if (!primary)
        gfx_draw_round_rect((uint32_t)r.x, (uint32_t)r.y, (uint32_t)r.w, (uint32_t)r.h, 7,
                            TH_CONTROL_EDGE);
    int tw = (int)gfx_text_width(label);
    gfx_text((uint32_t)(r.x + (r.w - tw) / 2), (uint32_t)(r.y + (r.h - 16) / 2), label,
             primary ? GFX_RGB(0xFF, 0xFF, 0xFF) : TH_TEXT, GFX_TRANSPARENT);
}

static void paint_dialog(files_t* f, int x, int y, int w, int h){
    // Dim what is behind the card so the window reads as waiting on it.
    gfx_blend_rect((uint32_t)x, (uint32_t)(y + TOP_H), (uint32_t)w, (uint32_t)(h - TOP_H - STATUS_H),
                   GFX_RGB(0, 0, 0), 90);

    dlg_t d = dlg_geom(f, w);
    rect_t c = { x + d.card.x, y + d.card.y, d.card.w, d.card.h };
    gfx_shadow(c.x, c.y, c.w, c.h, 10, TH_WIN_SHADOW);
    gfx_fill_round_rect((uint32_t)c.x, (uint32_t)c.y, (uint32_t)c.w, (uint32_t)c.h, 10, TH_PANEL);
    gfx_draw_round_rect((uint32_t)c.x, (uint32_t)c.y, (uint32_t)c.w, (uint32_t)c.h, 10, TH_PANEL_EDGE);

    const char* title = "";
    const char* ok = "OK";
    char line[FAT32_NAME_MAX + 40];
    switch (f->mode){
        case FM_NEWDIR:  title = "New folder";    ok = "Create"; break;
        case FM_NEWFILE: title = "New text file"; ok = "Create"; break;
        case FM_RENAME:  title = "Rename";        ok = "Rename"; break;
        default:         title = "Delete";        ok = "Delete"; break;
    }
    gfx_text((uint32_t)(c.x + 16), (uint32_t)(c.y + 16), title, TH_TEXT, GFX_TRANSPARENT);
    gfx_hline((uint32_t)(c.x + 16), (uint32_t)(c.y + 40), (uint32_t)(c.w - 32), TH_PANEL_EDGE);

    gfx_clip_set((uint32_t)(c.x + 16), (uint32_t)c.y, (uint32_t)(c.w - 32), (uint32_t)c.h);
    if (f->mode == FM_DELETE){
        int n = sel_count(f);
        const fat32_dirent_t* e = sole(f);
        if (e) ksnprintf(line, sizeof(line), "Delete \"%s\"?", e->name);
        else   ksnprintf(line, sizeof(line), "Delete these %d items?", n);
        gfx_text((uint32_t)(c.x + 16), (uint32_t)(c.y + 54), line, TH_TEXT, GFX_TRANSPARENT);

        int folders = 0;
        for (int i = 0; i < f->count; i++) if (f->marked[i] && f->entries[i].is_dir) folders++;
        gfx_text((uint32_t)(c.x + 16), (uint32_t)(c.y + 76),
                 folders ? "Folders are deleted with everything inside them." : "This cannot be undone.",
                 TH_TEXT_MUTED, GFX_TRANSPARENT);
        if (folders)
            gfx_text((uint32_t)(c.x + 16), (uint32_t)(c.y + 94), "This cannot be undone.",
                     TH_TEXT_MUTED, GFX_TRANSPARENT);
    } else {
        if (f->mode == FM_RENAME) ksnprintf(line, sizeof(line), "Rename \"%s\" to:", f->target);
        else if (f->mode == FM_NEWDIR) ksnprintf(line, sizeof(line), "Name for the new folder:");
        else ksnprintf(line, sizeof(line), "Name for the new file:");
        gfx_text((uint32_t)(c.x + 16), (uint32_t)(c.y + 50), line, TH_TEXT_MUTED, GFX_TRANSPARENT);

        rect_t fr = { x + d.field.x, y + d.field.y, d.field.w, d.field.h };
        gfx_fill_round_rect((uint32_t)fr.x, (uint32_t)fr.y, (uint32_t)fr.w, (uint32_t)fr.h, 6, TH_FIELD);
        gfx_draw_round_rect((uint32_t)fr.x, (uint32_t)fr.y, (uint32_t)fr.w, (uint32_t)fr.h, 6, TH_ACCENT);
        le_draw(&f->input, fr.x + 8, fr.y, fr.w - 16, fr.h, 1);

        if (f->dlg_err[0])
            gfx_text((uint32_t)(c.x + 16), (uint32_t)(c.y + 110), f->dlg_err, TH_ERROR, GFX_TRANSPARENT);
    }
    gfx_clip_reset();

    int mx = mouse_x(), my = mouse_y();
    rect_t okr = { x + d.ok.x, y + d.ok.y, d.ok.w, d.ok.h };
    rect_t cr  = { x + d.cancel.x, y + d.cancel.y, d.cancel.w, d.cancel.h };
    draw_btn(okr, ok, 1, f->mode == FM_DELETE, inside(okr, mx, my));
    draw_btn(cr, "Cancel", 0, 0, inside(cr, mx, my));
}

// True when the preview pane is showing, which changes how wide the list is
// and therefore where a click lands. One function so paint and hit-testing
// can never disagree about it.
static int preview_open(const files_t* f, int w){
    const fat32_dirent_t* e = sole(f);
    return (e && !e->is_dir && w > NAV_W + PREVIEW_W + 260);
}

static void paint(wm_window_t* win, files_t* f){
    int x, y, w, h;
    wm_client_rect(win, &x, &y, &w, &h);

    paint_toolbar(f, x, y, w);
    paint_cmdbar(f, x, y, w);

    int body_y = y + TOP_H;
    int body_h = h - TOP_H - STATUS_H;

    paint_nav(f, x, body_y, NAV_W, body_h);

    int list_x = x + NAV_W;
    int list_w = w - NAV_W;
    int pv = preview_open(f, w);
    if (pv) list_w -= PREVIEW_W;

    paint_header(list_x, body_y, list_w);
    paint_list(f, list_x, body_y + HEADER_H, list_w, body_h - HEADER_H);

    if (pv) paint_preview(f, list_x + list_w, body_y, PREVIEW_W, body_h);

    paint_status(f, x, y + h - STATUS_H, w);

    if (f->mode != FM_NONE) paint_dialog(f, x, y, w, h);
}

// ----------------------------------------------------------------- input

static void click_row(files_t* f, int row){
    int ctrl = kbd_ctrl_down(), shift = kbd_shift_down();

    if (shift && f->anchor >= 0){
        select_range(f, f->anchor, row);
        f->sel = row;
        f->click_row = -1;
        return;
    }
    if (ctrl){
        f->marked[row] = !f->marked[row];
        f->sel = f->anchor = row;
        load_preview(f);
        f->click_row = -1;
        return;
    }

    // Single click selects, double click opens -- the behaviour of the file
    // manager this is shaped after. The previous version opened on the second
    // click whenever it landed, which meant a folder opened by accident every
    // time somebody clicked the row they had already selected.
    int dbl = (row == f->click_row && ticks - f->click_when < DCLICK_TICKS);
    f->click_row  = row;
    f->click_when = ticks;

    select_only(f, row);
    if (dbl){ open_selected(f); f->click_row = -1; }
}

static void on_click(files_t* f, int cx, int cy, int w, int h){
    if (f->mode != FM_NONE){
        dlg_t d = dlg_geom(f, w);
        if (inside(d.ok, cx, cy)){ commit_dialog(f); return; }
        if (inside(d.cancel, cx, cy)){ f->mode = FM_NONE; return; }
        if (f->mode != FM_DELETE && inside(d.field, cx, cy)){
            le_click(&f->input, cx - (d.field.x + 8), kbd_shift_down());
            f->input_drag = 1;
        }
        return;                                     // the rest of the window is inert
    }

    // Navigation row
    if (cy < TOOLBAR_H){
        if (cy >= 9 && cy < 31){
            if (cx >= 10 && cx < 36){ go_back(f); return; }
            if (cx >= 62 && cx < 88){ go_up(f);   return; }
        }
        return;
    }

    // Command bar
    if (cy < TOP_H){
        for (int i = 0; i < C_N; i++)
            if (inside(cmd_rect(i, w), cx, cy) && cmd_enabled(f, i)){ run_cmd(f, i); return; }
        return;
    }
    if (cy >= h - STATUS_H) return;

    int body_y = TOP_H;

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
    if (row < 0 || row >= f->count){
        // Empty space below the last row clears the selection.
        if (!kbd_ctrl_down() && !kbd_shift_down()) select_only(f, -1);
        return;
    }
    click_row(f, row);
}

static void move_focus(files_t* f, int to, int visible){
    if (to < 0) to = 0;
    if (to >= f->count) to = f->count - 1;
    if (to < 0) return;
    if (kbd_shift_down() && f->anchor >= 0){
        select_range(f, f->anchor, to);
        f->sel = to;
    } else {
        select_only(f, to);
    }
    keep_row_visible(f, visible);
}

static void on_key(files_t* f, int key, int client_h){
    // The dialog owns the keyboard while it is up.
    if (f->mode != FM_NONE){
        if (key == '\n'){ commit_dialog(f); return; }
        if (key == 27){ f->mode = FM_NONE; return; }
        if (f->mode != FM_DELETE){
            f->dlg_err[0] = 0;
            le_key(&f->input, key);
        }
        return;
    }

    int visible = visible_rows(client_h);

    if (kbd_ctrl_down()){
        int k = (key >= 'A' && key <= 'Z') ? key + 32 : key;
        switch (k){
            case 'a': select_all(f); return;
            case 'c': do_copy(f, 0); return;
            case 'x': do_copy(f, 1); return;
            case 'v': do_paste(f);   return;
            case 'n': open_dialog(f, kbd_shift_down() ? FM_NEWDIR : FM_NEWFILE); return;
            default: return;
        }
    }

    switch (key){
        case KEY_UP:    move_focus(f, f->sel < 0 ? 0 : f->sel - 1, visible); break;
        case KEY_DOWN:  move_focus(f, f->sel < 0 ? 0 : f->sel + 1, visible); break;
        case KEY_HOME:  move_focus(f, 0, visible); break;
        case KEY_END:   move_focus(f, f->count - 1, visible); break;
        case KEY_PGUP:  move_focus(f, f->sel - visible, visible); break;
        case KEY_PGDN:  move_focus(f, f->sel + visible, visible); break;
        case '\n':      open_selected(f); break;
        case '\b':      go_up(f); break;
        case KEY_LEFT:  go_back(f); break;
        case KEY_RIGHT: open_selected(f); break;
        case KEY_DELETE: open_dialog(f, FM_DELETE); break;
        case KEY_F2:    open_dialog(f, FM_RENAME); break;
        default: break;
    }
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

        case WM_EV_MOUSE_MOVE:
            if (f->mode != FM_NONE && f->input_drag){
                int x, y, w, h;
                wm_client_rect(win, &x, &y, &w, &h);
                dlg_t d = dlg_geom(f, w);
                le_drag(&f->input, ev->x - (d.field.x + 8));
                wm_invalidate();
            }
            break;

        case WM_EV_MOUSE_UP:
            f->input_drag = 0;
            break;

        case WM_EV_WHEEL:
            if (f->mode == FM_NONE){
                f->scroll += ev->key * 3;
                if (f->scroll < 0) f->scroll = 0;
                wm_invalidate();
            }
            break;

        case WM_EV_KEY: {
            int x, y, w, h;
            wm_client_rect(win, &x, &y, &w, &h);
            on_key(f, ev->key, h);
            wm_invalidate();
        } break;

        case WM_EV_TICK:
            if (f->flash[0] && ticks - f->flash_when >= FLASH_TICKS){
                f->flash[0] = 0;
                wm_invalidate();
            }
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
    f->sel = f->anchor = -1;
    f->click_row = -1;

    f->preview = (uint8_t*)kmalloc(PREVIEW_CAP);

    shell_init();                       // idempotent; mounts the volume once
    f->mounted = shell_fs_ready();
    if (f->mounted){
        load_nav(f);
        load_dir(f, fat32_root_cluster());
    }

    wm_window_t* w = wm_open("Files", 100, 60, 860, 520, handler, f);
    if (w) wm_set_wheel(w, 1);
}
