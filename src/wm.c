// src/wm.c — compositing window manager
//
// The whole scene is repainted into gfx.c's back buffer every frame and
// blitted once. That is more pixels than a dirty-rectangle scheme would
// touch, but it removes the entire class of bugs where a window moves and
// leaves fragments behind, and at these resolutions the copy is cheap enough
// that the loop still spends most of its time asleep. What keeps it cheap is
// that the loop is event driven: nothing is painted at all until input
// arrives or an app calls wm_invalidate().
//
// Windows can be moved, resized from any edge or corner, maximised, snapped
// to half the screen, minimised and cycled with Ctrl+Tab. Geometry changes
// are funnelled through wm_set_geometry(), which is the single place that
// clamps to the minimum size and the work area and tells the app its client
// area moved -- so no path can resize a window without the app hearing about
// it.
#include <stdint.h>
#include "header/wm.h"
#include "header/theme.h"
#include "header/gfx.h"
#include "header/mouse.h"
#include "header/kbd.h"
#include "header/task.h"
#include "header/rtc.h"
#include "header/kstring.h"

extern volatile unsigned long long ticks;

static wm_window_t windows[WM_MAX_WINDOWS];

// Bottom-to-top paint order, holding indices into `windows`. An explicit
// order array beats storing a z number per window: raising a window is one
// splice, and painting is a straight walk with no sorting.
static int  order[WM_MAX_WINDOWS];
static int  order_n = 0;
static int  focused = -1;          // index into `windows`, or -1

static uint32_t next_win_id = 1;

static wm_root_paint_t root_paint = 0;
static wm_root_click_t root_click = 0;
static wm_root_key_t   root_key   = 0;

static int             bar_left_w = 0, bar_right_w = 0;
static wm_bar_paint_t  bar_left_paint = 0, bar_right_paint = 0;
static wm_bar_click_t  bar_left_click = 0, bar_right_click = 0;

static wm_overlay_paint_t overlay_paint = 0;
static wm_overlay_click_t overlay_click = 0;

static int running = 0;
static int dirty   = 1;

static int drag_win = -1, drag_dx = 0, drag_dy = 0;

// Resize in progress. The window's rect at the moment of the grab is kept so
// every motion event computes an absolute result from the original, rather
// than accumulating deltas -- which drifts, and which makes a window creep
// when it is pushed against its minimum size.
static int resize_win = -1, resize_edge = 0;
static int grab_mx, grab_my, grab_x, grab_y, grab_w, grab_h;

// Which snap the current drag would commit to if the button were released
// now. Drawn as an outline while dragging so the gesture is visible before it
// happens, rather than the window jumping somewhere unexpected on release.
static int snap_hint = 0;

static unsigned long long click_when = 0;
static int click_where_x = 0, click_where_y = 0, click_which = -1;

#define BTN_W    20
#define BTN_GAP  6
#define TAB_W    170
#define TAB_GAP  6

#define GRIP        5      // thickness of the resize border, in pixels
#define SNAP_EDGE   10     // how close to a screen edge a drag starts snapping
#define DCLICK_MS   45     // ticks; the PIT runs at 100 Hz

enum { SNAP_NONE = 0, SNAP_MAX, SNAP_LEFT, SNAP_RIGHT };
enum { EDGE_L = 1, EDGE_R = 2, EDGE_T = 4, EDGE_B = 8 };

static int idx_of(const wm_window_t* w){
    if (!w) return -1;
    int i = (int)(w - windows);
    return (i >= 0 && i < WM_MAX_WINDOWS && windows[i].used) ? i : -1;
}

void wm_invalidate(void){ dirty = 1; }

void wm_work_area(int* x, int* y, int* w, int* h){
    if (x) *x = 0;
    if (y) *y = 0;
    if (w) *w = (int)gfx_width();
    if (h) *h = (int)gfx_height() - WM_TASKBAR_H;
}

void wm_set_root(wm_root_paint_t paint, wm_root_click_t click, wm_root_key_t key){
    root_paint = paint;
    root_click = click;
    root_key   = key;
}

void wm_set_taskbar_ends(int left_w, int right_w,
                         wm_bar_paint_t lp, wm_bar_click_t lc,
                         wm_bar_paint_t rp, wm_bar_click_t rc){
    bar_left_w  = left_w;  bar_left_paint  = lp; bar_left_click  = lc;
    bar_right_w = right_w; bar_right_paint = rp; bar_right_click = rc;
}

void wm_set_overlay(wm_overlay_paint_t paint, wm_overlay_click_t click){
    overlay_paint = paint;
    overlay_click = click;
}

void wm_init(void){
    for (int i = 0; i < WM_MAX_WINDOWS; i++) windows[i].used = 0;
    order_n = 0;
    focused = -1;
    drag_win = -1;
    resize_win = -1;
    snap_hint = 0;
    dirty = 1;
}

static void order_raise(int idx){
    int at = -1;
    for (int i = 0; i < order_n; i++) if (order[i] == idx) { at = i; break; }
    if (at < 0) { if (order_n < WM_MAX_WINDOWS) order[order_n++] = idx; return; }
    for (int i = at; i < order_n - 1; i++) order[i] = order[i + 1];
    order[order_n - 1] = idx;
}

static void order_remove(int idx){
    int at = -1;
    for (int i = 0; i < order_n; i++) if (order[i] == idx) { at = i; break; }
    if (at < 0) return;
    for (int i = at; i < order_n - 1; i++) order[i] = order[i + 1];
    order_n--;
}

// ---------------------------------------------------------------- geometry

static void send(int idx, int type, int a, int b, int key);

void wm_client_rect(const wm_window_t* win, int* x, int* y, int* w, int* h){
    if (x) *x = win->x + WM_BORDER;
    if (y) *y = win->y + WM_TITLE_H;
    if (w) *w = win->w - 2 * WM_BORDER;
    if (h) *h = win->h - WM_TITLE_H - WM_BORDER;
}

void wm_set_geometry(wm_window_t* win, int x, int y, int w, int h){
    int i = idx_of(win);
    if (i < 0) return;

    int wax, way, waw, wah;
    wm_work_area(&wax, &way, &waw, &wah);

    if (w < WM_MIN_W) w = WM_MIN_W;
    if (h < WM_MIN_H) h = WM_MIN_H;
    if (w > waw) w = waw;
    if (h > wah) h = wah;

    // Keep the title bar reachable. A window whose bar is above the top of
    // the screen or below the taskbar can never be grabbed again, and the
    // only way back would be to close it from the task manager.
    if (y < way) y = way;
    if (y > way + wah - WM_TITLE_H) y = way + wah - WM_TITLE_H;
    if (x < wax - (w - 120)) x = wax - (w - 120);
    if (x > wax + waw - 120)  x = wax + waw - 120;

    int resized = (w != win->w || h != win->h);
    win->x = x; win->y = y; win->w = w; win->h = h;

    if (resized){
        int cw, ch;
        wm_client_rect(win, 0, 0, &cw, &ch);
        send(i, WM_EV_RESIZE, cw, ch, 0);
    }
    dirty = 1;
}

void wm_maximize(wm_window_t* win, int on){
    int i = idx_of(win);
    if (i < 0 || (!!win->maximized) == (!!on)) return;

    if (on){
        win->rx = win->x; win->ry = win->y;
        win->rw = win->w; win->rh = win->h;
        int x, y, w, h;
        wm_work_area(&x, &y, &w, &h);
        win->maximized = 1;
        wm_set_geometry(win, x, y, w, h);
    } else {
        win->maximized = 0;
        wm_set_geometry(win, win->rx, win->ry, win->rw, win->rh);
    }
}

void wm_minimize(wm_window_t* win, int on){
    int i = idx_of(win);
    if (i < 0) return;
    win->minimized = on ? 1 : 0;
    // Hand focus to the next window down rather than dropping it on the
    // floor: with focus at -1 the keyboard goes nowhere and the desktop
    // looks hung until something is clicked.
    if (on && focused == i){
        focused = -1;
        for (int k = order_n - 1; k >= 0; k--){
            int idx = order[k];
            if (windows[idx].used && !windows[idx].minimized){ focused = idx; break; }
        }
    }
    dirty = 1;
}

// Snaps to half the work area. Half-screen tiling is the one arrangement
// worth a gesture on a machine with no virtual desktops: it is what you want
// every time you read a page and type notes about it.
static void snap_apply(int idx, int snap){
    wm_window_t* w = &windows[idx];
    int x, y, ww, wh;
    wm_work_area(&x, &y, &ww, &wh);

    if (snap == SNAP_MAX){ wm_maximize(w, 1); return; }

    if (!w->maximized){ w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h; }
    w->maximized = 0;
    if (snap == SNAP_LEFT)  wm_set_geometry(w, x, y, ww / 2, wh);
    if (snap == SNAP_RIGHT) wm_set_geometry(w, x + ww / 2, y, ww - ww / 2, wh);
}

static void snap_rect(int snap, int* x, int* y, int* w, int* h){
    int ax, ay, aw, ah;
    wm_work_area(&ax, &ay, &aw, &ah);
    switch (snap){
        case SNAP_MAX:   *x = ax;          *y = ay; *w = aw;          *h = ah; break;
        case SNAP_LEFT:  *x = ax;          *y = ay; *w = aw / 2;      *h = ah; break;
        case SNAP_RIGHT: *x = ax + aw / 2; *y = ay; *w = aw - aw / 2; *h = ah; break;
        default:         *x = *y = *w = *h = 0; break;
    }
}

// ------------------------------------------------------------------- slots

wm_window_t* wm_open(const char* title, int x, int y, int w, int h,
                     wm_handler_t handler, void* user){
    int slot = -1;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) if (!windows[i].used) { slot = i; break; }
    if (slot < 0) return 0;

    // Cascade: apps ask for a fixed position, so opening two of the same one
    // used to stack them pixel-for-pixel and look like nothing had happened.
    for (int tries = 0; tries < WM_MAX_WINDOWS; tries++){
        int clash = 0;
        for (int i = 0; i < WM_MAX_WINDOWS; i++)
            if (windows[i].used && windows[i].x == x && windows[i].y == y){ clash = 1; break; }
        if (!clash) break;
        x += 26; y += 26;
    }

    wm_window_t* win = &windows[slot];
    win->used      = 1;
    win->id        = next_win_id++;
    win->minimized = 0;
    win->maximized = 0;
    win->x = x; win->y = y; win->w = w; win->h = h;
    win->rx = x; win->ry = y; win->rw = w; win->rh = h;
    win->handler   = handler;
    win->user      = user;
    strncpy(win->title, title ? title : "", WM_TITLE_MAX - 1);
    win->title[WM_TITLE_MAX - 1] = 0;

    order_raise(slot);
    focused = slot;
    dirty = 1;

    // Clamp into the work area now that the slot is live, so a window opened
    // larger than the screen arrives already usable.
    wm_set_geometry(win, x, y, w, h);
    return win;
}

void wm_close(wm_window_t* win){
    int i = idx_of(win);
    if (i < 0) return;

    if (win->handler) { wm_event_t ev = { WM_EV_CLOSE, 0, 0, 0 }; win->handler(win, &ev); }
    win->used = 0;
    order_remove(i);
    if (focused == i) focused = order_n ? order[order_n - 1] : -1;
    if (drag_win == i) drag_win = -1;
    if (resize_win == i) resize_win = -1;
    dirty = 1;
}

void wm_focus(wm_window_t* win){
    int i = idx_of(win);
    if (i < 0) return;
    windows[i].minimized = 0;
    order_raise(i);
    focused = i;
    dirty = 1;
}

int wm_is_focused(const wm_window_t* win){ return idx_of(win) == focused; }

int wm_window_count(void){
    int n = 0;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) if (windows[i].used) n++;
    return n;
}

// ------------------------------------------------------- outside interface

static int find_id(uint32_t id){
    for (int i = 0; i < WM_MAX_WINDOWS; i++)
        if (windows[i].used && windows[i].id == id) return i;
    return -1;
}

int wm_snapshot(wm_info_t* out, int max){
    int n = 0;
    // Topmost first: the order array runs bottom to top, so walk it backwards.
    for (int i = order_n - 1; i >= 0 && n < max; i--){
        int idx = order[i];
        if (!windows[idx].used) continue;
        wm_info_t* o = &out[n++];
        o->id = windows[idx].id;
        strncpy(o->title, windows[idx].title, WM_TITLE_MAX - 1);
        o->title[WM_TITLE_MAX - 1] = 0;
        o->focused   = (idx == focused && !windows[idx].minimized);
        o->minimized = windows[idx].minimized;
        o->maximized = windows[idx].maximized;
        o->w = windows[idx].w;
        o->h = windows[idx].h;
    }
    return n;
}

int wm_close_id(uint32_t id){
    int i = find_id(id);
    if (i < 0) return -1;
    wm_close(&windows[i]);
    return 0;
}

int wm_focus_id(uint32_t id){
    int i = find_id(id);
    if (i < 0) return -1;
    wm_focus(&windows[i]);
    return 0;
}

void wm_quit(void){ running = 0; }

// ------------------------------------------------------------------ paint

// Knocks the corner pixels out of a rectangle so windows read as rounded
// rather than as hard boxes. Three pixels is enough to be visible at this
// resolution without the corner looking chewed.
static void round_corners(int x, int y, int w, int h, uint32_t bg){
    static const int CUT[3] = { 3, 2, 1 };
    for (int i = 0; i < 3; i++){
        int n = CUT[i];
        gfx_fill_rect((uint32_t)x, (uint32_t)(y + i), (uint32_t)n, 1, bg);
        gfx_fill_rect((uint32_t)(x + w - n), (uint32_t)(y + i), (uint32_t)n, 1, bg);
        gfx_fill_rect((uint32_t)x, (uint32_t)(y + h - 1 - i), (uint32_t)n, 1, bg);
        gfx_fill_rect((uint32_t)(x + w - n), (uint32_t)(y + h - 1 - i), (uint32_t)n, 1, bg);
    }
}

// Three buttons now, right to left: close, maximise, minimise.
static void title_buttons(const wm_window_t* win, int* min_x, int* max_x, int* close_x){
    int cx = win->x + win->w - BTN_W - 6;
    int ax = cx - BTN_W - BTN_GAP;
    int mx = ax - BTN_W - BTN_GAP;
    if (min_x)   *min_x = mx;
    if (max_x)   *max_x = ax;
    if (close_x) *close_x = cx;
}

static void paint_frame(int idx, uint32_t desk_bg){
    wm_window_t* win = &windows[idx];
    int on = (idx == focused);

    gfx_clip_reset();

    gfx_fill_rect((uint32_t)win->x, (uint32_t)win->y, (uint32_t)win->w, WM_TITLE_H,
                  on ? TH_TITLE_ON : TH_TITLE_OFF);
    gfx_draw_rect((uint32_t)win->x, (uint32_t)win->y, (uint32_t)win->w, (uint32_t)win->h,
                  on ? TH_ACCENT_DIM : TH_WIN_EDGE);

    // A focused window gets a bright hairline under its title bar. It reads
    // as focus at a glance without needing a second title-bar colour that
    // fights with the palette.
    if (on)
        gfx_hline((uint32_t)win->x, (uint32_t)(win->y + WM_TITLE_H - 1),
                  (uint32_t)win->w, TH_ACCENT);
    else
        gfx_hline((uint32_t)win->x, (uint32_t)(win->y + WM_TITLE_H - 1),
                  (uint32_t)win->w, TH_WIN_EDGE);

    int bmin, bmax, bclose;
    title_buttons(win, &bmin, &bmax, &bclose);

    // Clip the title so a long one stops at the buttons instead of running
    // underneath them.
    gfx_clip_set((uint32_t)(win->x + 10), (uint32_t)win->y,
                 (uint32_t)(bmin - win->x - 16), WM_TITLE_H);
    gfx_text((uint32_t)(win->x + 10), (uint32_t)(win->y + 6), win->title,
             on ? TH_TEXT : TH_TEXT_MUTED, GFX_TRANSPARENT);
    gfx_clip_reset();

    int by = win->y + 5;

    // Minimise: a bar. Maximise: a box, or two offset boxes when the window
    // is already maximised. Close: a cross. All drawn rather than lettered,
    // so they do not depend on the font having a glyph for them.
    gfx_fill_rect((uint32_t)bmin, (uint32_t)by, BTN_W, 18, TH_PANEL);
    gfx_fill_rect((uint32_t)(bmin + 5), (uint32_t)(by + 12), 10, 2, TH_TEXT_MUTED);

    gfx_fill_rect((uint32_t)bmax, (uint32_t)by, BTN_W, 18, TH_PANEL);
    if (win->maximized){
        gfx_draw_rect((uint32_t)(bmax + 5), (uint32_t)(by + 6), 9, 8, TH_TEXT_MUTED);
        gfx_draw_rect((uint32_t)(bmax + 8), (uint32_t)(by + 3), 9, 8, TH_TEXT_MUTED);
    } else {
        gfx_draw_rect((uint32_t)(bmax + 5), (uint32_t)(by + 4), 11, 10, TH_TEXT_MUTED);
    }

    gfx_fill_rect((uint32_t)bclose, (uint32_t)by, BTN_W, 18, on ? TH_CLOSE : TH_PANEL);
    for (int i = 0; i < 8; i++){
        gfx_put_pixel((uint32_t)(bclose + 6 + i), (uint32_t)(by + 5 + i), TH_TEXT);
        gfx_put_pixel((uint32_t)(bclose + 13 - i), (uint32_t)(by + 5 + i), TH_TEXT);
    }

    int clx, cly, clw, clh;
    wm_client_rect(win, &clx, &cly, &clw, &clh);
    gfx_fill_rect((uint32_t)clx, (uint32_t)cly, (uint32_t)clw, (uint32_t)clh, TH_WIN_BG);

    if (win->handler){
        gfx_clip_set((uint32_t)clx, (uint32_t)cly, (uint32_t)clw, (uint32_t)clh);
        wm_event_t ev = { WM_EV_PAINT, 0, 0, 0 };
        win->handler(win, &ev);
        gfx_clip_reset();
    }

    // Three ticks in the bottom-right corner: without them the resize border
    // is invisible and nobody would think to reach for it.
    if (!win->maximized){
        for (int i = 0; i < 3; i++){
            int o = 3 + i * 4;
            gfx_fill_rect((uint32_t)(win->x + win->w - o - 2),
                          (uint32_t)(win->y + win->h - 4), 2, 2,
                          on ? TH_ACCENT_DIM : TH_WIN_EDGE);
            gfx_fill_rect((uint32_t)(win->x + win->w - 4),
                          (uint32_t)(win->y + win->h - o - 2), 2, 2,
                          on ? TH_ACCENT_DIM : TH_WIN_EDGE);
        }
    }

    round_corners(win->x, win->y, win->w, win->h, desk_bg);
}

static void paint_snap_hint(void){
    if (!snap_hint) return;
    int x, y, w, h;
    snap_rect(snap_hint, &x, &y, &w, &h);
    gfx_clip_reset();
    gfx_draw_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, TH_ACCENT);
    gfx_draw_rect((uint32_t)(x + 1), (uint32_t)(y + 1), (uint32_t)(w - 2), (uint32_t)(h - 2),
                  TH_ACCENT_DIM);
    gfx_draw_rect((uint32_t)(x + 2), (uint32_t)(y + 2), (uint32_t)(w - 4), (uint32_t)(h - 4),
                  TH_ACCENT_DIM);
}

static void paint_taskbar(void){
    uint32_t W = gfx_width(), H = gfx_height();
    int y = (int)(H - WM_TASKBAR_H);

    gfx_clip_reset();
    gfx_fill_rect(0, (uint32_t)y, W, WM_TASKBAR_H, TH_TASKBAR);
    gfx_hline(0, (uint32_t)y, W, TH_TASKBAR_EDGE);

    if (bar_left_paint)  bar_left_paint(0, y, bar_left_w, WM_TASKBAR_H);
    if (bar_right_paint) bar_right_paint((int)W - bar_right_w, y, bar_right_w, WM_TASKBAR_H);

    // One button per open window, so a minimised window is still reachable.
    int bx    = bar_left_w + 8;
    int limit = (int)W - bar_right_w - 8;

    for (int i = 0; i < order_n; i++){
        int idx = order[i];
        if (!windows[idx].used) continue;
        if (bx + TAB_W > limit) break;

        int on  = (idx == focused && !windows[idx].minimized);
        int min = windows[idx].minimized;

        gfx_fill_rect((uint32_t)bx, (uint32_t)(y + 7), TAB_W, WM_TASKBAR_H - 14,
                      on ? TH_HOVER : TH_TASKBAR);
        if (on) gfx_hline((uint32_t)bx, (uint32_t)(y + 7), TAB_W, TH_ACCENT);
        else    gfx_draw_rect((uint32_t)bx, (uint32_t)(y + 7), TAB_W, WM_TASKBAR_H - 14,
                              TH_TASKBAR_EDGE);

        gfx_clip_set((uint32_t)(bx + 8), (uint32_t)y, TAB_W - 16, WM_TASKBAR_H);
        gfx_text((uint32_t)(bx + 10), (uint32_t)(y + 14), windows[idx].title,
                 on ? TH_TEXT : (min ? TH_TEXT_DIM : TH_TEXT_MUTED), GFX_TRANSPARENT);
        gfx_clip_reset();

        bx += TAB_W + TAB_GAP;
    }
}

// 12x19 arrow. '#' is the black outline, '.' the white fill, space is
// transparent — an outlined cursor stays visible over both the dark desktop
// and a lighter panel, which a single-colour triangle does not.
static const char* const CURSOR[19] = {
    "#           ",
    "##          ",
    "#.#         ",
    "#..#        ",
    "#...#       ",
    "#....#      ",
    "#.....#     ",
    "#......#    ",
    "#.......#   ",
    "#........#  ",
    "#.........# ",
    "#......#####",
    "#...#..#    ",
    "#..# #..#   ",
    "#.#  #..#   ",
    "##    #..#  ",
    "#     #..#  ",
    "       ###  ",
    "            ",
};

static void paint_cursor(int mx, int my){
    gfx_clip_reset();
    for (int y = 0; y < 19; y++){
        const char* row = CURSOR[y];
        for (int x = 0; x < 12; x++){
            char c = row[x];
            if (c == '#')      gfx_put_pixel((uint32_t)(mx + x), (uint32_t)(my + y), GFX_RGB(0x05, 0x07, 0x0A));
            else if (c == '.') gfx_put_pixel((uint32_t)(mx + x), (uint32_t)(my + y), GFX_RGB(0xFF, 0xFF, 0xFF));
        }
    }
}

static void compose(int mx, int my){
    gfx_clip_reset();
    if (root_paint) root_paint();
    else gfx_vgradient(0, 0, gfx_width(), gfx_height(), TH_DESK_TOP, TH_DESK_BOT);

    for (int i = 0; i < order_n; i++){
        int idx = order[i];
        if (windows[idx].used && !windows[idx].minimized) paint_frame(idx, TH_DESK_BOT);
    }

    paint_snap_hint();
    paint_taskbar();
    if (overlay_paint) overlay_paint();
    paint_cursor(mx, my);
}

// ------------------------------------------------------------------ input

static int hit_test(int x, int y){
    for (int i = order_n - 1; i >= 0; i--){
        int idx = order[i];
        wm_window_t* w = &windows[idx];
        if (!w->used || w->minimized) continue;
        // The grip sticks out past the frame, so a window is grabbable a few
        // pixels beyond its own edge -- otherwise hitting a 1px border with a
        // mouse is a test of patience.
        if (x >= w->x - GRIP && x < w->x + w->w + GRIP &&
            y >= w->y - GRIP && y < w->y + w->h + GRIP) return idx;
    }
    return -1;
}

// Which edges of `w` the point is on, as a mask of EDGE_*. Zero means the
// point is inside the window proper.
static int edge_hit(const wm_window_t* w, int x, int y){
    if (w->maximized) return 0;
    int e = 0;
    if (x >= w->x - GRIP && x < w->x + GRIP)                  e |= EDGE_L;
    if (x >= w->x + w->w - GRIP && x < w->x + w->w + GRIP)    e |= EDGE_R;
    if (y >= w->y - GRIP && y < w->y + GRIP)                  e |= EDGE_T;
    if (y >= w->y + w->h - GRIP && y < w->y + w->h + GRIP)    e |= EDGE_B;
    return e;
}

// Returns -2 when the point is not in the taskbar at all, -1 for taskbar
// background, otherwise the window index whose button was hit.
static int taskbar_hit(int x, int y){
    uint32_t W = gfx_width(), H = gfx_height();
    if (y < (int)(H - WM_TASKBAR_H)) return -2;

    int bx    = bar_left_w + 8;
    int limit = (int)W - bar_right_w - 8;

    for (int i = 0; i < order_n; i++){
        int idx = order[i];
        if (!windows[idx].used) continue;
        if (bx + TAB_W > limit) break;
        if (x >= bx && x < bx + TAB_W) return idx;
        bx += TAB_W + TAB_GAP;
    }
    return -1;
}

static void send(int idx, int type, int a, int b, int key){
    if (idx < 0 || !windows[idx].used || !windows[idx].handler) return;
    // Pointer events arrive in screen coordinates and are delivered relative
    // to the client area; WM_EV_RESIZE already carries a size, so it passes
    // through untouched.
    if (type == WM_EV_RESIZE){
        wm_event_t ev = { type, a, b, key };
        windows[idx].handler(&windows[idx], &ev);
        return;
    }
    int cx, cy, cw, ch;
    wm_client_rect(&windows[idx], &cx, &cy, &cw, &ch);
    wm_event_t ev = { type, a - cx, b - cy, key };
    windows[idx].handler(&windows[idx], &ev);
}

static void begin_resize(int idx, int edge, int mx, int my){
    resize_win  = idx;
    resize_edge = edge;
    grab_mx = mx; grab_my = my;
    grab_x = windows[idx].x; grab_y = windows[idx].y;
    grab_w = windows[idx].w; grab_h = windows[idx].h;
}

static void on_press(int mx, int my){
    // The launcher menu floats above everything, so it gets first refusal on
    // every click; otherwise a click meant to dismiss it would also land on
    // whatever is underneath.
    if (overlay_click && overlay_click(mx, my)){ dirty = 1; return; }

    int tb = taskbar_hit(mx, my);
    if (tb != -2){
        uint32_t W = gfx_width();
        if (mx < bar_left_w && bar_left_click){ bar_left_click(mx, my); dirty = 1; return; }
        if (mx >= (int)W - bar_right_w && bar_right_click){ bar_right_click(mx, my); dirty = 1; return; }
        if (tb >= 0){
            if (tb == focused && !windows[tb].minimized) wm_minimize(&windows[tb], 1);
            else wm_focus(&windows[tb]);
            dirty = 1;
        }
        return;
    }

    int idx = hit_test(mx, my);
    if (idx < 0){
        if (root_click) root_click(mx, my);
        dirty = 1;
        return;
    }

    wm_window_t* w = &windows[idx];
    if (idx != focused) wm_focus(w);

    // Edges first: the top-left corner of a window is both a resize corner
    // and part of the title bar, and resize is the more specific gesture.
    int edge = edge_hit(w, mx, my);
    if (edge){ begin_resize(idx, edge, mx, my); return; }

    int bmin, bmax, bclose;
    title_buttons(w, &bmin, &bmax, &bclose);
    int by = w->y + 5;

    if (my >= by && my < by + 18){
        if (mx >= bclose && mx < bclose + BTN_W){ wm_close(w); return; }
        if (mx >= bmax && mx < bmax + BTN_W){ wm_maximize(w, !w->maximized); return; }
        if (mx >= bmin && mx < bmin + BTN_W){ wm_minimize(w, 1); return; }
    }

    if (my < w->y + WM_TITLE_H){
        // Double-click the title bar to toggle maximise, the way every other
        // desktop does it.
        int dbl = (click_which == idx && ticks - click_when < DCLICK_MS
                   && mx - click_where_x < 6 && click_where_x - mx < 6
                   && my - click_where_y < 6 && click_where_y - my < 6);
        click_which = idx; click_when = ticks; click_where_x = mx; click_where_y = my;
        if (dbl){ wm_maximize(w, !w->maximized); click_which = -1; return; }

        // Dragging a maximised window restores it under the cursor, keeping
        // the pointer at the same fraction along the title bar. Without
        // this the window jumps out from under the pointer the instant the
        // drag begins.
        if (w->maximized){
            int oldw = w->w;
            wm_maximize(w, 0);
            drag_dx = (oldw > 0) ? (mx * w->w) / oldw : w->w / 2;
            if (drag_dx > w->w - 40) drag_dx = w->w - 40;
            if (drag_dx < 20) drag_dx = 20;
            drag_dy = WM_TITLE_H / 2;
            drag_win = idx;
            wm_set_geometry(w, mx - drag_dx, my - drag_dy, w->w, w->h);
            return;
        }

        drag_win = idx;
        drag_dx  = mx - w->x;
        drag_dy  = my - w->y;
        return;
    }

    send(idx, WM_EV_MOUSE_DOWN, mx, my, 0);
    dirty = 1;
}

static void on_release(int mx, int my){
    if (resize_win >= 0){ resize_win = -1; resize_edge = 0; return; }

    if (drag_win >= 0){
        int idx = drag_win;
        drag_win = -1;
        if (snap_hint){ snap_apply(idx, snap_hint); snap_hint = 0; dirty = 1; }
        return;
    }
    int idx = hit_test(mx, my);
    if (idx >= 0) send(idx, WM_EV_MOUSE_UP, mx, my, 0);
}

// Cycles focus through the open windows, topmost-last, so repeated presses
// walk the stack instead of flipping between the same two.
static void cycle_focus(void){
    if (order_n < 2) return;
    for (int i = 0; i < order_n; i++){
        int idx = order[i];
        if (windows[idx].used){ wm_focus(&windows[idx]); return; }
    }
}

static void do_resize(int mx, int my){
    wm_window_t* w = &windows[resize_win];
    int nx = grab_x, ny = grab_y, nw = grab_w, nh = grab_h;
    int dx = mx - grab_mx, dy = my - grab_my;

    if (resize_edge & EDGE_R) nw = grab_w + dx;
    if (resize_edge & EDGE_B) nh = grab_h + dy;
    if (resize_edge & EDGE_L){
        // Clamp the delta rather than the result, or the left edge keeps
        // moving after the window has already hit its minimum width.
        if (grab_w - dx < WM_MIN_W) dx = grab_w - WM_MIN_W;
        nx = grab_x + dx; nw = grab_w - dx;
    }
    if (resize_edge & EDGE_T){
        if (grab_h - dy < WM_MIN_H) dy = grab_h - WM_MIN_H;
        ny = grab_y + dy; nh = grab_h - dy;
    }

    w->maximized = 0;
    wm_set_geometry(w, nx, ny, nw, nh);
    w->rx = w->x; w->ry = w->y; w->rw = w->w; w->rh = w->h;
}

void wm_run(void){
    running = 1;
    dirty   = 1;

    int prev_btn = 0;
    int prev_mx  = mouse_x(), prev_my = mouse_y();
    unsigned long long last_tick = ticks;

    while (running){
        int mx = mouse_x(), my = mouse_y();
        int btn = mouse_left_button();

        if (mx != prev_mx || my != prev_my){
            if (resize_win >= 0){
                do_resize(mx, my);
            } else if (drag_win >= 0){
                wm_window_t* w = &windows[drag_win];
                wm_set_geometry(w, mx - drag_dx, my - drag_dy, w->w, w->h);

                // Snap gestures, resolved against the pointer rather than the
                // window: the pointer is what the user is aiming, and a wide
                // window's own edge reaches the screen border long before
                // they meant anything by it.
                int aw = (int)gfx_width();
                if (my <= SNAP_EDGE)                snap_hint = SNAP_MAX;
                else if (mx <= SNAP_EDGE)           snap_hint = SNAP_LEFT;
                else if (mx >= aw - 1 - SNAP_EDGE)  snap_hint = SNAP_RIGHT;
                else                                snap_hint = SNAP_NONE;
            } else {
                int idx = hit_test(mx, my);
                if (idx >= 0 && btn) send(idx, WM_EV_MOUSE_MOVE, mx, my, 0);
            }
            prev_mx = mx; prev_my = my;
            dirty = 1;
        }

        if (btn != prev_btn){
            if (btn) on_press(mx, my);
            else     on_release(mx, my);
            prev_btn = btn;
            dirty = 1;
        }

        // The wheel is delivered as repeated arrow keys rather than as its
        // own event type. Every scrollable app already handles Up/Down, so
        // this makes the wheel work everywhere without touching any of them.
        int wheel = mouse_wheel_take();
        if (wheel && focused >= 0 && windows[focused].used){
            int steps = wheel < 0 ? -wheel : wheel;
            if (steps > 8) steps = 8;
            for (int s = 0; s < steps * 3; s++)
                send(focused, WM_EV_KEY, 0, 0, wheel > 0 ? KEY_DOWN : KEY_UP);
            dirty = 1;
        }

        int key;
        while ((key = kbd_getc()) >= 0){
            // The desktop gets first look, so a global shortcut works no
            // matter which window happens to be focused.
            if (root_key && root_key(key)){ dirty = 1; continue; }

            // Window-management keys are the manager's own, taken before the
            // focused app sees them. Ctrl+Tab rather than Alt+Tab only
            // because the keyboard driver tracks ctrl and shift, not alt.
            if (kbd_ctrl_down() && key == '\t'){ cycle_focus(); dirty = 1; continue; }
            if (focused >= 0 && windows[focused].used){
                if (kbd_ctrl_down() && (key == KEY_UP || key == KEY_DOWN)){
                    wm_window_t* w = &windows[focused];
                    if (key == KEY_UP) wm_maximize(w, 1);
                    else if (w->maximized) wm_maximize(w, 0);
                    else wm_minimize(w, 1);
                    dirty = 1;
                    continue;
                }
                if (kbd_ctrl_down() && key == KEY_LEFT) { snap_apply(focused, SNAP_LEFT);  dirty = 1; continue; }
                if (kbd_ctrl_down() && key == KEY_RIGHT){ snap_apply(focused, SNAP_RIGHT); dirty = 1; continue; }
            }

            if (focused >= 0 && windows[focused].used) send(focused, WM_EV_KEY, 0, 0, key);
            dirty = 1;
        }

        // A 4 Hz heartbeat drives the taskbar clock and anything that shows
        // live values (the task manager, a download in progress).
        if (ticks - last_tick >= 25){
            last_tick = ticks;
            for (int i = 0; i < WM_MAX_WINDOWS; i++)
                if (windows[i].used && windows[i].handler){
                    wm_event_t ev = { WM_EV_TICK, 0, 0, 0 };
                    windows[i].handler(&windows[i], &ev);
                }
            dirty = 1;
        }

        if (dirty){
            gfx_use_backbuffer(1);
            compose(mx, my);
            gfx_present();
            gfx_use_backbuffer(0);
            dirty = 0;
        }

        task_sleep(10);
    }
}
