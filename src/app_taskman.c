// src/app_taskman.c — Task Manager
//
// Shows the scheduler's own table: every kernel thread, its state, and how
// many timer slices it has been given. The slice column is the interesting
// one — watching it climb for a busy task and stall for a sleeping one is
// the clearest evidence that the round-robin scheduler is really running.
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

#define ROW_H 20

typedef struct {
    task_info_t rows[TASK_MAX];
    int         count;
    int         selected;
} taskman_t;

static void refresh(taskman_t* t){
    t->count = task_snapshot(t->rows, TASK_MAX);
    if (t->selected >= t->count) t->selected = t->count - 1;
    if (t->selected < 0) t->selected = 0;
}

static uint32_t state_colour(int state){
    switch (state){
        case 2:  return TH_OK;      // running
        case 1:  return TH_LINK;    // ready
        case 3:  return TH_WARN;    // sleeping
        default: return TH_TEXT_DIM;
    }
}

static void paint(wm_window_t* win, taskman_t* t){
    int x, y, w, h;
    wm_client_rect(win, &x, &y, &w, &h);

    char line[96];

    gfx_fill_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, 28, TH_PANEL);
    gfx_text((uint32_t)(x + 12), (uint32_t)(y + 6), "ID",     WM_COL_TEXT_MUTED, GFX_TRANSPARENT);
    gfx_text((uint32_t)(x + 56), (uint32_t)(y + 6), "NAME",   WM_COL_TEXT_MUTED, GFX_TRANSPARENT);
    gfx_text((uint32_t)(x + 210), (uint32_t)(y + 6), "STATE", WM_COL_TEXT_MUTED, GFX_TRANSPARENT);
    gfx_text((uint32_t)(x + 320), (uint32_t)(y + 6), "SLICES", WM_COL_TEXT_MUTED, GFX_TRANSPARENT);
    gfx_hline((uint32_t)x, (uint32_t)(y + 27), (uint32_t)w, TH_PANEL_EDGE);

    int ry = y + 32;
    for (int i = 0; i < t->count; i++){
        if (ry + ROW_H > y + h - 72) break;

        if (i == t->selected)
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
    int fy = y + h - 66;
    gfx_hline((uint32_t)x, (uint32_t)fy, (uint32_t)w, TH_PANEL_EDGE);

    size_t hu = 0, hf = 0;
    kheap_stats(&hu, &hf);
    ksnprintf(line, sizeof(line), "kheap %u KB used / %u KB free",
              (unsigned)(hu / 1024u), (unsigned)(hf / 1024u));
    gfx_text((uint32_t)(x + 12), (uint32_t)(fy + 8), line, WM_COL_TEXT_MUTED, GFX_TRANSPARENT);

    ksnprintf(line, sizeof(line), "physical %u KB free of %u KB",
              pmm_free_frames() * (PMM_FRAME_SIZE / 1024u), pmm_ram_top() / 1024u);
    gfx_text((uint32_t)(x + 12), (uint32_t)(fy + 26), line, WM_COL_TEXT_MUTED, GFX_TRANSPARENT);

    // On its own line rather than sharing one with the physical-memory
    // figure: at a narrow window width the two overlapped.
    gfx_text((uint32_t)(x + 12), (uint32_t)(fy + 44),
             "up/down select   k kill", WM_COL_TEXT_MUTED, GFX_TRANSPARENT);
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
            int row = (ev->y - 32 + 2) / ROW_H;
            if (row >= 0 && row < t->count){ t->selected = row; wm_invalidate(); }
        } break;

        case WM_EV_KEY:
            if (ev->key == KEY_UP   && t->selected > 0)              t->selected--;
            else if (ev->key == KEY_DOWN && t->selected < t->count-1) t->selected++;
            else if (ev->key == 'k' || ev->key == 'K'){
                if (t->selected >= 0 && t->selected < t->count){
                    task_kill(t->rows[t->selected].id);
                    refresh(t);
                }
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
    refresh(t);
    wm_open("Task Manager", 180, 120, 470, 380, handler, t);
}
