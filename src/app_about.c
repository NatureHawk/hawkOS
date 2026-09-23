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
    gfx_text((uint32_t)x, (uint32_t)*y, key, TH_TEXT_MUTED, GFX_TRANSPARENT);
    gfx_text((uint32_t)(x + 130), (uint32_t)*y, val, TH_TEXT, GFX_TRANSPARENT);
    *y += 22;
}

static void paint(wm_window_t* win){
    int x, y, w, h;
    wm_client_rect(win, &x, &y, &w, &h);

    gfx_fill_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, 74, TH_PANEL);
    gfx_hline((uint32_t)x, (uint32_t)(y + 73), (uint32_t)w, TH_PANEL_EDGE);

    // The mark, drawn rather than lettered: a disc with the hawk's beak and
    // crest cut out of it in the panel colour, so it follows the appearance
    // instead of being a fixed pair of colours.
    gfx_fill_circle(x + 40, y + 37, 20, TH_ACCENT);
    for (int i = 0; i < 8; i++)
        gfx_fill_rect((uint32_t)(x + 44), (uint32_t)(y + 31 + i / 2), (uint32_t)(10 - i), 1,
                      TH_PANEL);
    for (int i = 0; i < 7; i++)
        gfx_fill_rect((uint32_t)(x + 26 - i / 2), (uint32_t)(y + 27 + i), 3, 1, TH_PANEL);

    gfx_text((uint32_t)(x + 76), (uint32_t)(y + 22), "hawkOS", TH_TEXT, GFX_TRANSPARENT);
    gfx_text((uint32_t)(x + 76), (uint32_t)(y + 42), "a hobby operating system, built from scratch",
             TH_TEXT_MUTED, GFX_TRANSPARENT);

    char buf[80], vendor[13], brand[49];
    int ry = y + 92;

    cpuid_vendor(vendor);
    cpuid_brand(brand);

    row(x + 16, &ry, "CPU", brand[0] ? brand : vendor);

    ksnprintf(buf, sizeof(buf), "%ux%u 32bpp", gfx_width(), gfx_height());
    row(x + 16, &ry, "Display", buf);

    uint32_t total_kb = pmm_total_kb();
    uint32_t used_kb  = pmm_used_kb();
    ksnprintf(buf, sizeof(buf), "%u MB used of %u MB", used_kb / 1024u, total_kb / 1024u);
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
             "Open an app from the dock at the bottom of the screen.",
             TH_TEXT_MUTED, GFX_TRANSPARENT);
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
    wm_open("About hawkOS", 420, 150, 540, 372, handler, a);
}
