// src/app_about.c — About / System Information
#include <stdint.h>
#include "header/apps.h"
#include "header/wm.h"
#include "header/theme.h"
#include "header/gfx.h"
#include "header/kheap.h"
#include "header/kstring.h"
#include "header/pmm.h"
#include "header/cpuid.h"
#include "header/rtc.h"
#include "header/task.h"
#include "header/font.h"

extern volatile unsigned long long ticks;

typedef struct { int dummy; } about_t;

static void row(int x, int* y, const char* key, const char* val){
    gfx_text((uint32_t)x, (uint32_t)*y, key, WM_COL_TEXT_MUTED, GFX_TRANSPARENT);
    gfx_text((uint32_t)(x + 130), (uint32_t)*y, val, WM_COL_TEXT_DARK, GFX_TRANSPARENT);
    *y += 22;
}

static void paint(wm_window_t* win){
    int x, y, w, h;
    wm_client_rect(win, &x, &y, &w, &h);

    gfx_fill_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, 62, TH_PANEL);
    gfx_fill_circle(x + 36, y + 31, 18, WM_COL_ACCENT);
    gfx_fill_rect((uint32_t)(x + 30), (uint32_t)(y + 24), 14, 4, TH_PANEL);
    gfx_fill_rect((uint32_t)(x + 30), (uint32_t)(y + 32), 9, 4, TH_PANEL);
    gfx_text((uint32_t)(x + 68), (uint32_t)(y + 16), "hawkOS", WM_COL_ACCENT, GFX_TRANSPARENT);
    gfx_text((uint32_t)(x + 68), (uint32_t)(y + 36), "a hobby operating system, built from scratch",
             WM_COL_TEXT_LIGHT, GFX_TRANSPARENT);

    char buf[80], vendor[13], brand[49];
    int ry = y + 78;

    cpuid_vendor(vendor);
    cpuid_brand(brand);

    row(x + 16, &ry, "CPU", brand[0] ? brand : vendor);

    ksnprintf(buf, sizeof(buf), "%ux%u 32bpp", gfx_width(), gfx_height());
    row(x + 16, &ry, "Display", buf);

    uint32_t total_kb = pmm_ram_top() / 1024u;
    uint32_t free_kb  = pmm_free_frames() * (PMM_FRAME_SIZE / 1024u);
    ksnprintf(buf, sizeof(buf), "%u MB total, %u MB free", total_kb / 1024u, free_kb / 1024u);
    row(x + 16, &ry, "Memory", buf);

    size_t hu = 0, hf = 0;
    kheap_stats(&hu, &hf);
    ksnprintf(buf, sizeof(buf), "%u KB used of %u KB",
              (unsigned)(hu / 1024u), (unsigned)((hu + hf) / 1024u));
    row(x + 16, &ry, "Kernel heap", buf);

    task_info_t rows[TASK_MAX];
    int n = task_snapshot(rows, TASK_MAX);
    ksnprintf(buf, sizeof(buf), "%d live", n);
    row(x + 16, &ry, "Tasks", buf);

    unsigned long long t = ticks;
    ksnprintf(buf, sizeof(buf), "%u m %u s", (unsigned)(t / 6000), (unsigned)((t / 100) % 60));
    row(x + 16, &ry, "Uptime", buf);

    rtc_time_t now;
    rtc_read(&now);
    ksnprintf(buf, sizeof(buf), "%u-%02u-%02u %02u:%02u UTC",
              now.year, now.month, now.day, now.hour, now.min);
    row(x + 16, &ry, "Clock", buf);

    gfx_hline((uint32_t)(x + 16), (uint32_t)(ry + 6), (uint32_t)(w - 32), TH_PANEL_EDGE);
    gfx_text((uint32_t)(x + 16), (uint32_t)(ry + 18),
             "Click an icon on the left to open an app.", WM_COL_TEXT_MUTED, GFX_TRANSPARENT);
}

static void handler(wm_window_t* win, const wm_event_t* ev){
    switch (ev->type){
        case WM_EV_PAINT: paint(win); break;
        case WM_EV_TICK:  break;                  // the compositor repaints anyway
        case WM_EV_CLOSE:
            if (win->user) { kfree(win->user); win->user = 0; }
            break;
        default: break;
    }
}

void app_about_open(void){
    about_t* a = (about_t*)kmalloc(sizeof(about_t));
    if (!a) return;
    memset(a, 0, sizeof(*a));
    wm_open("About hawkOS", 420, 160, 520, 350, handler, a);
}
