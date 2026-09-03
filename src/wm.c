// src/wm.c — compositing window manager
//
// The whole scene is repainted into gfx.c's back buffer every frame and
// blitted once. That is more pixels than a dirty-rectangle scheme would
// touch, but it removes the entire class of bugs where a window moves and
// leaves fragments behind, and at these resolutions the copy is cheap enough
// that the loop still spends most of its time asleep. What keeps it cheap is
// that the loop is event driven: nothing is painted at all until input
// arrives or an app calls wm_invalidate().
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

#define BTN_W    20
#define BTN_GAP  6
#define TAB_W    170
#define TAB_GAP  6

static int idx_of(const wm_window_t* w){
    if (!w) return -1;
    int i = (int)(w - windows);
    return (i >= 0 && i < WM_MAX_WINDOWS && windows[i].used) ? i : -1;
}

void wm_invalidate(void){ dirty = 1; }

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

wm_window_t* wm_open(const char* title, int x, int y, int w, int h,
                     wm_handler_t handler, void* user){
    int slot = -1;
    for (int i = 0; i < WM_MAX_WINDOWS; i++) if (!windows[i].used) { slot = i; break; }
    if (slot < 0) return 0;

    wm_window_t* win = &windows[slot];
    win->used      = 1;
    win->minimized = 0;
    win->x = x; win->y = y; win->w = w; win->h = h;
    win->handler   = handler;
    win->user      = user;
    strncpy(win->title, title ? title : "", WM_TITLE_MAX - 1);
    win->title[WM_TITLE_MAX - 1] = 0;

    order_raise(slot);
    focused = slot;
    dirty = 1;
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

void wm_client_rect(const wm_window_t* win, int* x, int* y, int* w, int* h){
    if (x) *x = win->x + WM_BORDER;
    if (y) *y = win->y + WM_TITLE_H;
    if (w) *w = win->w - 2 * WM_BORDER;
    if (h) *h = win->h - WM_TITLE_H - WM_BORDER;
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

static void title_buttons(const wm_window_t* win, int* min_x, int* close_x){
    int cx = win->x + win->w - BTN_W - 6;
    int mx = cx - BTN_W - BTN_GAP;
    if (min_x)   *min_x = mx;
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

    gfx_text((uint32_t)(win->x + 10), (uint32_t)(win->y + 6), win->title,
             on ? TH_TEXT : TH_TEXT_MUTED, GFX_TRANSPARENT);

    int mx, cx;
    title_buttons(win, &mx, &cx);
    int by = win->y + 5;

    // Minimise: a single bar. Close: a cross. Both drawn rather than
    // lettered, so they do not depend on the font having a glyph for them.
    gfx_fill_rect((uint32_t)mx, (uint32_t)by, BTN_W, 18, TH_PANEL);
    gfx_fill_rect((uint32_t)(mx + 5), (uint32_t)(by + 12), 10, 2, TH_TEXT_MUTED);

    gfx_fill_rect((uint32_t)cx, (uint32_t)by, BTN_W, 18, on ? TH_CLOSE : TH_PANEL);
    for (int i = 0; i < 8; i++){
        gfx_put_pixel((uint32_t)(cx + 6 + i), (uint32_t)(by + 5 + i), TH_TEXT);
        gfx_put_pixel((uint32_t)(cx + 13 - i), (uint32_t)(by + 5 + i), TH_TEXT);
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

    round_corners(win->x, win->y, win->w, win->h, desk_bg);
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
        if (x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h) return idx;
    }
    return -1;
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

static void send(int idx, int type, int sx, int sy, int key){
    if (idx < 0 || !windows[idx].used || !windows[idx].handler) return;
    int cx, cy, cw, ch;
    wm_client_rect(&windows[idx], &cx, &cy, &cw, &ch);
    wm_event_t ev = { type, sx - cx, sy - cy, key };
    windows[idx].handler(&windows[idx], &ev);
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
            if (tb == focused && !windows[tb].minimized) windows[tb].minimized = 1;
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

    int bmin, bclose;
    title_buttons(w, &bmin, &bclose);
    int by = w->y + 5;

    if (my >= by && my < by + 18){
        if (mx >= bclose && mx < bclose + BTN_W){ wm_close(w); return; }
        if (mx >= bmin && mx < bmin + BTN_W){ w->minimized = 1; dirty = 1; return; }
    }

    if (my < w->y + WM_TITLE_H){                     // title bar: start a drag
        drag_win = idx;
        drag_dx  = mx - w->x;
        drag_dy  = my - w->y;
        return;
    }

    send(idx, WM_EV_MOUSE_DOWN, mx, my, 0);
    dirty = 1;
}

static void on_release(int mx, int my){
    if (drag_win >= 0){ drag_win = -1; return; }
    int idx = hit_test(mx, my);
    if (idx >= 0) send(idx, WM_EV_MOUSE_UP, mx, my, 0);
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
            if (drag_win >= 0){
                wm_window_t* w = &windows[drag_win];
                w->x = mx - drag_dx;
                w->y = my - drag_dy;
                // Keep the title bar reachable: a window dragged off the top
                // or past the taskbar could never be grabbed again.
                if (w->y < 0) w->y = 0;
                if (w->y > (int)gfx_height() - WM_TASKBAR_H - WM_TITLE_H)
                    w->y = (int)gfx_height() - WM_TASKBAR_H - WM_TITLE_H;
                if (w->x < -(w->w - 120)) w->x = -(w->w - 120);
                if (w->x > (int)gfx_width() - 120) w->x = (int)gfx_width() - 120;
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
