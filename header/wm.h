#pragma once
#include <stdint.h>
#include "header/theme.h"

#define WM_MAX_WINDOWS  10
#define WM_TITLE_H      28
#define WM_BORDER       1
#define WM_TASKBAR_H    44
#define WM_TITLE_MAX    28

// Floor on a window's outer size. Below this the title bar's three buttons
// start colliding with the title, and no app's client area is usable.
#define WM_MIN_W        260
#define WM_MIN_H        160

typedef struct wm_window wm_window_t;

enum {
    WM_EV_PAINT = 0,    // repaint the client area; clip is already set to it
    WM_EV_KEY,          // ev.key, focused window only
    WM_EV_MOUSE_DOWN,   // ev.x/ev.y are client-relative
    WM_EV_MOUSE_UP,
    WM_EV_MOUSE_MOVE,
    WM_EV_TICK,         // ~4 Hz, every window, for anything self-refreshing
    WM_EV_CLOSE,        // last call before the slot is released

    // The client area changed size: ev.x and ev.y are its new width and
    // height. Apps that measure at paint time need do nothing; apps holding
    // laid-out geometry (the browser's display list) have to rebuild it here,
    // because paint happens with the clip already set and is too late to
    // reflow.
    WM_EV_RESIZE
};

typedef struct {
    int type;
    int x, y;
    int key;
} wm_event_t;

typedef void (*wm_handler_t)(wm_window_t* win, const wm_event_t* ev);

struct wm_window {
    int          used;
    uint32_t     id;                  // stable identity; slots get reused, ids do not
    int          minimized;
    int          maximized;
    int          x, y, w, h;          // outer rect, including title bar
    int          rx, ry, rw, rh;      // geometry to restore a maximised window to
    char         title[WM_TITLE_MAX];
    wm_handler_t handler;
    void*        user;                // per-app state, owned by the app
};

void          wm_init(void);
wm_window_t*  wm_open(const char* title, int x, int y, int w, int h,
                      wm_handler_t handler, void* user);
void          wm_close(wm_window_t* win);
void          wm_focus(wm_window_t* win);
int           wm_is_focused(const wm_window_t* win);
int           wm_window_count(void);

void          wm_maximize(wm_window_t* win, int on);
void          wm_minimize(wm_window_t* win, int on);

// Resizes and/or moves a window, clamped to the minimum size and the work
// area, and tells the app if its client area changed.
void          wm_set_geometry(wm_window_t* win, int x, int y, int w, int h);

// The screen minus the taskbar: what a maximised or snapped window fills.
void          wm_work_area(int* x, int* y, int* w, int* h);

// Read-only view of the open windows, for anything that wants to list or act
// on them from outside -- the task manager, chiefly. Windows are addressed by
// id rather than by pointer so a caller can hold on to one across a repaint
// without risking a freed slot.
typedef struct {
    uint32_t id;
    char     title[WM_TITLE_MAX];
    int      focused;
    int      minimized;
    int      maximized;
    int      w, h;
} wm_info_t;

int  wm_snapshot(wm_info_t* out, int max);   // topmost first
int  wm_close_id(uint32_t id);               // 0 on success, -1 if no such window
int  wm_focus_id(uint32_t id);

// Marks the frame dirty. The compositor is idle until something asks for a
// repaint, so an app that changes its own state must call this or the change
// will not appear until the next mouse move.
void wm_invalidate(void);

// Client area in screen coordinates — what an app should draw into.
void wm_client_rect(const wm_window_t* win, int* x, int* y, int* w, int* h);

// The desktop underneath the windows: wallpaper, icons and the taskbar's
// extra furniture. Supplied by desktop.c so the window manager itself stays
// free of any policy about what the background looks like, what clicking it
// does, or what sits in the tray.
typedef void (*wm_root_paint_t)(void);
typedef void (*wm_root_click_t)(int x, int y);
typedef int  (*wm_root_key_t)(int key);      // returns 1 if it consumed the key
void wm_set_root(wm_root_paint_t paint, wm_root_click_t click, wm_root_key_t key);

// Taskbar hooks: the desktop owns the launcher button on the left and the
// status area on the right; the window manager owns the window buttons in
// between and tells the desktop how much room it has.
typedef void (*wm_bar_paint_t)(int x, int y, int w, int h);
typedef int  (*wm_bar_click_t)(int x, int y);   // returns 1 if handled
void wm_set_taskbar_ends(int left_w, int right_w,
                         wm_bar_paint_t left_paint,  wm_bar_click_t left_click,
                         wm_bar_paint_t right_paint, wm_bar_click_t right_click);

// Painted last, above everything including the taskbar — used for the
// launcher menu, which has to float over whatever is on screen.
typedef void (*wm_overlay_paint_t)(void);
typedef int  (*wm_overlay_click_t)(int x, int y);
void wm_set_overlay(wm_overlay_paint_t paint, wm_overlay_click_t click);

void wm_run(void);      // compositor loop; returns when wm_quit() is called
void wm_quit(void);

// Legacy palette names, now aliases onto the shared dark theme in theme.h.
// Keeping them means every app did not have to be edited when the system
// went dark, and new code can use either spelling.
#define WM_COL_DESK_TOP    TH_DESK_TOP
#define WM_COL_DESK_BOT    TH_DESK_BOT
#define WM_COL_TASKBAR     TH_TASKBAR
#define WM_COL_ACCENT      TH_ACCENT
#define WM_COL_WIN_BG      TH_WIN_BG
#define WM_COL_WIN_EDGE    TH_WIN_EDGE
#define WM_COL_TITLE_ON    TH_TITLE_ON
#define WM_COL_TITLE_OFF   TH_TITLE_OFF
#define WM_COL_TEXT_LIGHT  TH_TEXT
#define WM_COL_TEXT_DARK   TH_TEXT
#define WM_COL_TEXT_MUTED  TH_TEXT_MUTED
#define WM_COL_SEL         TH_SELECT
