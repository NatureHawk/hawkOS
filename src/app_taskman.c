// src/app_taskman.c — Task Manager
//
// Two tables, because the system has two different things a user might mean
// by "what is running".
//
// APPS are windows. Every app on this system is a wm_window_t whose handler
// runs inside the compositor loop on task 0 -- they are not threads, and they
// never were. Killing a row here closes the window, which is what "end task"
// has to mean for something with a title bar.
//
// TASKS are the scheduler's own kernel threads. The slice column is the
// interesting one: watching it climb for a busy task and stall for a sleeping
// one is the clearest evidence that the round-robin scheduler is really
// running. Killing one of these does not close any window, because no window
// belongs to one -- which is exactly why the app table above it exists.
#include <stdint.h>
#include "header/apps.h"
#include "header/wm.h"
#include "header/theme.h"
#include "header/gfx.h"
#include "header/task.h"
#include "header/kheap.h"
#include "header/kstring.h"
#include "header/pmm.h"
#include "header/kbd.h"
#include "header/font.h"

#define ROW_H     20
#define HEAD_H    22
#define FOOTER_H  70

typedef struct {
    wm_info_t   apps[WM_MAX_WINDOWS];
    int         app_n;
    task_info_t rows[TASK_MAX];
    int         task_n;

    // One selection across both tables: indices 0..app_n-1 are apps, the rest
    // are tasks. A single cursor means up and down just work across the
    // boundary instead of needing a "switch pane" key.
    int         selected;

    uint32_t    self_id;        // this window, so it can be marked
} taskman_t;

static int total_rows(taskman_t* t){ return t->app_n + t->task_n; }

static void refresh(taskman_t* t){
    t->app_n  = wm_snapshot(t->apps, WM_MAX_WINDOWS);
    t->task_n = task_snapshot(t->rows, TASK_MAX);

    int n = total_rows(t);
    if (t->selected >= n) t->selected = n - 1;
    if (t->selected < 0)  t->selected = 0;
}

static uint32_t state_colour(int state){
    switch (state){
        case 2:  return TH_OK;      // running
        case 1:  return TH_LINK;    // ready
        case 3:  return TH_WARN;    // sleeping
        default: return TH_TEXT_DIM;
    }
}

static void section(int x, int y, int w, const char* label, const char* c2, const char* c3){
    gfx_fill_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, HEAD_H, TH_PANEL);
    gfx_text((uint32_t)(x + 12), (uint32_t)(y + 3), label, TH_ACCENT, GFX_TRANSPARENT);
    if (c2) gfx_text((uint32_t)(x + 210), (uint32_t)(y + 3), c2, WM_COL_TEXT_MUTED, GFX_TRANSPARENT);
    if (c3) gfx_text((uint32_t)(x + 320), (uint32_t)(y + 3), c3, WM_COL_TEXT_MUTED, GFX_TRANSPARENT);
    gfx_hline((uint32_t)x, (uint32_t)(y + HEAD_H - 1), (uint32_t)w, TH_PANEL_EDGE);
}

static void paint(wm_window_t* win, taskman_t* t){
    int x, y, w, h;
    wm_client_rect(win, &x, &y, &w, &h);

    char line[96];
    int  limit = y + h - FOOTER_H;
    int  row   = 0;                       // running index into the selection
    int  ry    = y;

    // ------------------------------------------------------------- apps
    section(x, ry, w, "APPS", "STATE", "SIZE");
    ry += HEAD_H + 4;

    for (int i = 0; i < t->app_n && ry + ROW_H <= limit; i++, row++){
        if (row == t->selected)
            gfx_fill_rect((uint32_t)(x + 4), (uint32_t)(ry - 2), (uint32_t)(w - 8), ROW_H, WM_COL_SEL);

        int self = (t->apps[i].id == t->self_id);
        gfx_text((uint32_t)(x + 12), (uint32_t)ry, t->apps[i].title,
                 self ? TH_TEXT_MUTED : (t->apps[i].focused ? TH_ACCENT : TH_TEXT),
                 GFX_TRANSPARENT);

        const char* st = t->apps[i].minimized ? "minimised"
                       : t->apps[i].maximized ? "maximised"
                       : t->apps[i].focused   ? "focused" : "open";
        gfx_fill_rect((uint32_t)(x + 210), (uint32_t)(ry + 4), 8, 8,
                      t->apps[i].minimized ? TH_TEXT_DIM : TH_OK);
        gfx_text((uint32_t)(x + 224), (uint32_t)ry, st, WM_COL_TEXT_DARK, GFX_TRANSPARENT);

        ksnprintf(line, sizeof(line), "%dx%d", t->apps[i].w, t->apps[i].h);
        gfx_text((uint32_t)(x + 320), (uint32_t)ry, line, WM_COL_TEXT_DARK, GFX_TRANSPARENT);

        ry += ROW_H;
    }
    if (t->app_n == 0){
        gfx_text((uint32_t)(x + 12), (uint32_t)ry, "none", TH_TEXT_DIM, GFX_TRANSPARENT);
        ry += ROW_H;
    }

    // ------------------------------------------------------------ tasks
    ry += 6;
    if (ry + HEAD_H <= limit){
        section(x, ry, w, "KERNEL TASKS", "STATE", "SLICES");
        ry += HEAD_H + 4;
    }

    for (int i = 0; i < t->task_n && ry + ROW_H <= limit; i++, row++){
        if (row == t->selected)
            gfx_fill_rect((uint32_t)(x + 4), (uint32_t)(ry - 2), (uint32_t)(w - 8), ROW_H, WM_COL_SEL);

        ksnprintf(line, sizeof(line), "%u", t->rows[i].id);
        gfx_text((uint32_t)(x + 12), (uint32_t)ry, line, WM_COL_TEXT_DARK, GFX_TRANSPARENT);

        // The running task is marked with the accent colour. It used to use
        // the title-bar colour, which is nearly the selection colour under
        // the dark theme and left the name unreadable on the selected row.
        gfx_text((uint32_t)(x + 56), (uint32_t)ry, t->rows[i].name,
                 t->rows[i].is_current ? TH_ACCENT : TH_TEXT, GFX_TRANSPARENT);

        gfx_fill_rect((uint32_t)(x + 210), (uint32_t)(ry + 4), 8, 8, state_colour(t->rows[i].state));
        gfx_text((uint32_t)(x + 224), (uint32_t)ry, task_state_name(t->rows[i].state),
                 WM_COL_TEXT_DARK, GFX_TRANSPARENT);

        ksnprintf(line, sizeof(line), "%u", t->rows[i].slices);
        gfx_text((uint32_t)(x + 320), (uint32_t)ry, line, WM_COL_TEXT_DARK, GFX_TRANSPARENT);

        ry += ROW_H;
    }

    // Footer: the memory numbers, because "is anything leaking?" is the
    // other question this window exists to answer.
    int fy = y + h - FOOTER_H + 4;
    gfx_hline((uint32_t)x, (uint32_t)fy, (uint32_t)w, TH_PANEL_EDGE);

    size_t hu = 0, hf = 0;
    kheap_stats(&hu, &hf);
    ksnprintf(line, sizeof(line), "kheap %u KB used / %u KB free",
              (unsigned)(hu / 1024u), (unsigned)(hf / 1024u));
    gfx_text((uint32_t)(x + 12), (uint32_t)(fy + 8), line, WM_COL_TEXT_MUTED, GFX_TRANSPARENT);

    ksnprintf(line, sizeof(line), "system %u KB used of %u KB",
              pmm_used_kb(), pmm_total_kb());
    gfx_text((uint32_t)(x + 12), (uint32_t)(fy + 26), line, WM_COL_TEXT_MUTED, GFX_TRANSPARENT);

    // On its own line rather than sharing one with the memory figure: at a
    // narrow window width the two overlapped.
    gfx_text((uint32_t)(x + 12), (uint32_t)(fy + 44),
             "up/down select   enter show   k end", WM_COL_TEXT_MUTED, GFX_TRANSPARENT);
}

// Ends whatever the cursor is on. For an app that means closing its window;
// for a kernel task it means task_kill, which refuses on the kernel and idle
// tasks -- killing either would take the desktop down with it.
static void end_selected(wm_window_t* win, taskman_t* t){
    if (t->selected < 0) return;

    if (t->selected < t->app_n){
        uint32_t id = t->apps[t->selected].id;
        // Closing this window from inside its own handler would free the
        // state the rest of this call is standing on. Let wm_close run, then
        // return immediately without touching `t` again.
        if (id == t->self_id){ wm_close(win); return; }
        wm_close_id(id);
        refresh(t);
        return;
    }

    int ti = t->selected - t->app_n;
    if (ti >= 0 && ti < t->task_n){
        task_kill(t->rows[ti].id);
        refresh(t);
    }
}

// Maps a click's y to a selection index, mirroring the paint layout. Kept
// next to paint() on purpose: the two have to agree, and a click that
// selects the wrong row is the kind of bug that is invisible in a screenshot.
static int row_at(wm_window_t* win, taskman_t* t, int cy){
    int x, y, w, h;
    wm_client_rect(win, &x, &y, &w, &h);
    (void)x; (void)w;

    int ry = HEAD_H + 4;                       // client-relative
    int n  = t->app_n ? t->app_n : 1;
    if (cy < ry) return -1;
    if (cy < ry + n * ROW_H)
        return t->app_n ? (cy - ry) / ROW_H : -1;

    ry += n * ROW_H + 6 + HEAD_H + 4;
    if (cy < ry) return -1;
    int i = (cy - ry) / ROW_H;
    if (i >= t->task_n) return -1;
    return t->app_n + i;
}

static void handler(wm_window_t* win, const wm_event_t* ev){
    taskman_t* t = (taskman_t*)win->user;
    if (!t) return;

    switch (ev->type){
        case WM_EV_PAINT:
            paint(win, t);
            break;

        case WM_EV_TICK:
            refresh(t);
            break;

        case WM_EV_MOUSE_DOWN: {
            int row = row_at(win, t, ev->y);
            if (row >= 0){ t->selected = row; wm_invalidate(); }
        } break;

        case WM_EV_KEY:
            if (ev->key == KEY_UP && t->selected > 0) t->selected--;
            else if (ev->key == KEY_DOWN && t->selected < total_rows(t) - 1) t->selected++;
            else if (ev->key == '\n'){
                // Bring the selected app to the front. The task manager is
                // the only place a minimised window can be raised by name.
                if (t->selected >= 0 && t->selected < t->app_n)
                    wm_focus_id(t->apps[t->selected].id);
            }
            else if (ev->key == 'k' || ev->key == 'K'){
                end_selected(win, t);
                return;                    // `t` may be gone
            }
            wm_invalidate();
            break;

        case WM_EV_CLOSE:
            kfree(t);
            win->user = 0;
            break;

        default: break;
    }
}

void app_taskman_open(void){
    taskman_t* t = (taskman_t*)kmalloc(sizeof(taskman_t));
    if (!t) return;
    memset(t, 0, sizeof(*t));

    wm_window_t* win = wm_open("Task Manager", 180, 120, 470, 440, handler, t);
    if (!win){ kfree(t); return; }
    t->self_id = win->id;
    refresh(t);
}
