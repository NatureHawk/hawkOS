// src/desktop.c — the desktop shell: wallpaper, icons, launcher and tray
//
// This is the policy layer that sits on top of the window manager. wm.c
// knows how to stack, drag, focus and paint windows but nothing about what
// the background looks like, what is on it, or what lives at either end of
// the taskbar; everything specific to hawkOS's desktop lives here.
#include <stdint.h>
#include "header/desktop.h"
#include "header/wm.h"
#include "header/theme.h"
#include "header/gfx.h"
#include "header/apps.h"
#include "header/mouse.h"
#include "header/kbd.h"
#include "header/console.h"
#include "header/kstring.h"
#include "header/font.h"
#include "header/rtc.h"
#include "header/net.h"
#include "header/pmm.h"
#include "header/task.h"

extern volatile unsigned long long ticks;

#define ICON_W    104
#define ICON_H    96
#define ICON_ART  44
#define ICON_X0   28
#define ICON_Y0   30
#define ICON_GAPY 10

#define LAUNCH_W  148      // taskbar's left region
#define TRAY_W    390      // taskbar's right region

#define MENU_W    260
#define MENU_ROW  40

typedef void (*icon_open_t)(void);
typedef void (*icon_art_t)(int cx, int cy);

typedef struct {
    const char* label;
    icon_art_t  art;
    icon_open_t open;
} icon_t;

// ------------------------------------------------------------ pictograms
// Drawn from primitives rather than stored as bitmaps: at this size a folder
// is four rectangles, and keeping them as code means they follow the palette
// instead of needing re-exporting whenever a colour changes.

static void art_files(int cx, int cy){
    int x = cx - 22, y = cy - 16;
    gfx_fill_rect((uint32_t)x, (uint32_t)y, 18, 6, GFX_RGB(0xC2, 0x94, 0x24));
    gfx_fill_rect((uint32_t)x, (uint32_t)(y + 5), 44, 30, TH_ACCENT);
    gfx_fill_rect((uint32_t)(x + 4), (uint32_t)(y + 11), 36, 3, GFX_RGB(0xC2, 0x94, 0x24));
    gfx_fill_rect((uint32_t)(x + 4), (uint32_t)(y + 18), 26, 3, GFX_RGB(0xC2, 0x94, 0x24));
}

static void art_term(int cx, int cy){
    int x = cx - 23, y = cy - 17;
    gfx_fill_rect((uint32_t)x, (uint32_t)y, 46, 34, GFX_RGB(0x0B, 0x10, 0x16));
    gfx_draw_rect((uint32_t)x, (uint32_t)y, 46, 34, TH_WIN_EDGE);
    gfx_fill_rect((uint32_t)x, (uint32_t)y, 46, 7, TH_TITLE_ON);
    gfx_text((uint32_t)(x + 6), (uint32_t)(y + 13), ">_", TH_OK, GFX_TRANSPARENT);
}

static void art_taskman(int cx, int cy){
    int x = cx - 22, y = cy - 17;
    gfx_fill_rect((uint32_t)x, (uint32_t)y, 44, 34, GFX_RGB(0x11, 0x18, 0x20));
    gfx_draw_rect((uint32_t)x, (uint32_t)y, 44, 34, TH_WIN_EDGE);
    static const int bars[5] = { 8, 20, 13, 27, 17 };
    for (int i = 0; i < 5; i++)
        gfx_fill_rect((uint32_t)(x + 5 + i * 7), (uint32_t)(y + 30 - bars[i]),
                      5, (uint32_t)bars[i], TH_ACCENT);
}

static void art_browser(int cx, int cy){
    gfx_fill_circle(cx, cy, 21, GFX_RGB(0x1B, 0x4B, 0x78));
    gfx_draw_circle(cx, cy, 21, TH_LINK);
    // Meridians and the equator: enough to read as a globe at 44 px.
    gfx_draw_circle(cx, cy, 10, TH_LINK);
    gfx_hline((uint32_t)(cx - 21), (uint32_t)cy, 42, TH_LINK);
    gfx_vline((uint32_t)cx, (uint32_t)(cy - 21), 42, TH_LINK);
}

static void art_about(int cx, int cy){
    gfx_fill_circle(cx, cy, 20, GFX_RGB(0x2A, 0x35, 0x44));
    gfx_draw_circle(cx, cy, 20, TH_TEXT_MUTED);
    gfx_fill_rect((uint32_t)(cx - 2), (uint32_t)(cy - 12), 5, 5, TH_TEXT);
    gfx_fill_rect((uint32_t)(cx - 2), (uint32_t)(cy - 3), 5, 15, TH_TEXT);
}

static const icon_t ICONS[] = {
    { "Files",        art_files,   app_files_open   },
    { "Terminal",     art_term,    app_term_open    },
    { "Task Manager", art_taskman, app_taskman_open },
    { "Browser",      art_browser, app_browser_open },
    { "About",        art_about,   app_about_open   },
};
#define ICON_COUNT ((int)(sizeof(ICONS) / sizeof(ICONS[0])))

static int menu_open = 0;

static void icon_cell(int i, int* x, int* y){
    *x = ICON_X0;
    *y = ICON_Y0 + i * (ICON_H + ICON_GAPY);
}

// ---------------------------------------------------------------- desktop

static void desktop_paint(void){
    uint32_t W = gfx_width(), H = gfx_height();

    gfx_vgradient(0, 0, W, H, TH_DESK_TOP, TH_DESK_BOT);

    // A faint horizon band keeps the wallpaper from reading as a flat fill
    // without costing a real image.
    gfx_fill_rect(0, H * 2 / 3, W, 1, GFX_RGB(0x1E, 0x2A, 0x38));

    for (int i = 0; i < ICON_COUNT; i++){
        int x, y;
        icon_cell(i, &x, &y);

        ICONS[i].art(x + ICON_W / 2, y + 6 + ICON_ART / 2);

        uint32_t tw = gfx_text_width(ICONS[i].label);
        gfx_text((uint32_t)(x + (ICON_W - (int)tw) / 2), (uint32_t)(y + ICON_H - 28),
                 ICONS[i].label, TH_TEXT, GFX_TRANSPARENT);
    }

    const char* tag = "hawkOS v0.7";
    gfx_text(W - 14 - gfx_text_width(tag), H - WM_TASKBAR_H - 24, tag,
             TH_TEXT_DIM, GFX_TRANSPARENT);
}

static void desktop_click(int mx, int my){
    if (menu_open){ menu_open = 0; return; }

    for (int i = 0; i < ICON_COUNT; i++){
        int x, y;
        icon_cell(i, &x, &y);
        if (mx >= x && mx < x + ICON_W && my >= y && my < y + ICON_H){
            if (ICONS[i].open) ICONS[i].open();
            return;
        }
    }
}

static int desktop_key(int key){
    if (key == 27 && menu_open){ menu_open = 0; return 1; }     // Esc closes the menu
    if (key == KEY_F1){ menu_open = !menu_open; return 1; }
    return 0;
}

// --------------------------------------------------------------- launcher

static void launcher_paint(int x, int y, int w, int h){
    int on = menu_open;
    gfx_fill_rect((uint32_t)(x + 6), (uint32_t)(y + 7), (uint32_t)(w - 12), (uint32_t)(h - 14),
                  on ? TH_HOVER : TH_TASKBAR);
    if (on) gfx_hline((uint32_t)(x + 6), (uint32_t)(y + 7), (uint32_t)(w - 12), TH_ACCENT);

    // A four-pane glyph, drawn rather than lettered.
    int gx = x + 18, gy = y + h / 2 - 7;
    for (int r = 0; r < 2; r++)
        for (int c = 0; c < 2; c++)
            gfx_fill_rect((uint32_t)(gx + c * 8), (uint32_t)(gy + r * 8), 6, 6, TH_ACCENT);

    gfx_text((uint32_t)(x + 44), (uint32_t)(y + h / 2 - 8), "hawkOS", TH_TEXT, GFX_TRANSPARENT);
}

static int launcher_click(int x, int y){
    (void)x; (void)y;
    menu_open = !menu_open;
    return 1;
}

static void menu_paint(void){
    if (!menu_open) return;

    uint32_t H = gfx_height();
    int h = ICON_COUNT * MENU_ROW + 46;
    int x = 8;
    int y = (int)H - WM_TASKBAR_H - h - 6;

    gfx_fill_rect((uint32_t)x, (uint32_t)y, MENU_W, (uint32_t)h, TH_WIN_BG);
    gfx_draw_rect((uint32_t)x, (uint32_t)y, MENU_W, (uint32_t)h, TH_WIN_EDGE);
    gfx_fill_rect((uint32_t)x, (uint32_t)y, MENU_W, 3, TH_ACCENT);

    gfx_text((uint32_t)(x + 14), (uint32_t)(y + 14), "Applications", TH_TEXT_MUTED, GFX_TRANSPARENT);
    gfx_hline((uint32_t)(x + 10), (uint32_t)(y + 36), MENU_W - 20, TH_PANEL_EDGE);

    for (int i = 0; i < ICON_COUNT; i++){
        int ry = y + 44 + i * MENU_ROW;
        ICONS[i].art(x + 30, ry + MENU_ROW / 2 - 2);
        gfx_text((uint32_t)(x + 62), (uint32_t)(ry + MENU_ROW / 2 - 8),
                 ICONS[i].label, TH_TEXT, GFX_TRANSPARENT);
    }
}

static int menu_click(int mx, int my){
    if (!menu_open) return 0;

    uint32_t H = gfx_height();
    int h = ICON_COUNT * MENU_ROW + 46;
    int x = 8;
    int y = (int)H - WM_TASKBAR_H - h - 6;

    if (mx < x || mx >= x + MENU_W || my < y || my >= y + h){
        // A click outside the menu dismisses it, and is swallowed so it does
        // not also activate whatever it landed on.
        menu_open = 0;
        return 1;
    }

    for (int i = 0; i < ICON_COUNT; i++){
        int ry = y + 44 + i * MENU_ROW;
        if (my >= ry && my < ry + MENU_ROW){
            menu_open = 0;
            if (ICONS[i].open) ICONS[i].open();
            return 1;
        }
    }
    menu_open = 0;
    return 1;
}

// -------------------------------------------------------------------- tray

static const char* const MONTHS[13] = {
    "", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static void tray_paint(int x, int y, int w, int h){
    char buf[40];

    // Three fixed columns rather than positions measured from each other:
    // the memory bar used to be placed relative to the IP text, so a longer
    // address pushed it into the clock.
    const int COL_NET  = 14;
    const int COL_MEM  = 160;
    const int COL_TIME = 268;

    // Network: a status dot plus the address, so "is it online" is readable
    // at a glance and "what address" is there when it matters.
    int nx = x + COL_NET;
    int ny = y + h / 2 - 8;

    if (net_configured()){
        char ip[16];
        gfx_fill_circle(nx + 4, ny + 8, 4, TH_OK);
        net_ip_str(net_local_ip(), ip);
        gfx_text((uint32_t)(nx + 14), (uint32_t)ny, ip, TH_TEXT_MUTED, GFX_TRANSPARENT);
    } else {
        gfx_fill_circle(nx + 4, ny + 8, 4, net_link_up() ? TH_WARN : TH_ERROR);
        gfx_text((uint32_t)(nx + 14), (uint32_t)ny,
                 net_link_up() ? "no lease" : "offline", TH_TEXT_MUTED, GFX_TRANSPARENT);
    }

    // Memory used, against the 128 MB budget the project is built to. This
    // reads pmm_used_kb() rather than the frame count: the frame count cannot
    // see kmalloc, so a meter built on it sits at whatever the kernel image
    // reserved at boot and never moves again.
    uint32_t total_kb = pmm_total_kb();
    uint32_t used_kb  = pmm_used_kb();
    uint32_t used_pct = total_kb ? (used_kb * 100u / total_kb) : 0;

    int mx = x + COL_MEM;
    ksnprintf(buf, sizeof(buf), "%u%%", used_pct);
    gfx_text((uint32_t)mx, (uint32_t)ny, buf,
             used_pct > 85 ? TH_WARN : TH_TEXT_MUTED, GFX_TRANSPARENT);

    int bar_x = mx + 36;
    gfx_fill_rect((uint32_t)bar_x, (uint32_t)(ny + 5), 48, 6, TH_PANEL);
    gfx_fill_rect((uint32_t)bar_x, (uint32_t)(ny + 5), (uint32_t)(48 * used_pct / 100), 6,
                  used_pct > 85 ? TH_WARN : TH_ACCENT_DIM);
    gfx_draw_rect((uint32_t)bar_x, (uint32_t)(ny + 5), 48, 6, TH_PANEL_EDGE);

    gfx_vline((uint32_t)(x + COL_TIME - 18), (uint32_t)(y + 12), (uint32_t)(h - 24),
              TH_PANEL_EDGE);

    // Clock: time on top in the accent colour, date underneath, both right
    // aligned against the screen edge.
    rtc_time_t t;
    rtc_read(&t);

    ksnprintf(buf, sizeof(buf), "%02u:%02u:%02u", t.hour, t.min, t.sec);
    uint32_t tw = gfx_text_width(buf);
    gfx_text((uint32_t)(x + w - 14 - (int)tw), (uint32_t)(y + 6), buf, TH_TEXT, GFX_TRANSPARENT);

    ksnprintf(buf, sizeof(buf), "%02u %s %u", t.day,
              MONTHS[t.month >= 1 && t.month <= 12 ? t.month : 0], t.year);
    tw = gfx_text_width(buf);
    gfx_text((uint32_t)(x + w - 14 - (int)tw), (uint32_t)(y + 24), buf,
             TH_TEXT_DIM, GFX_TRANSPARENT);
}

static int tray_click(int x, int y){
    (void)x; (void)y;
    return 0;
}

// --------------------------------------------------------------------- run

void desktop_run(void){
    if (!gfx_available()) return;

    wm_init();
    wm_set_root(desktop_paint, desktop_click, desktop_key);
    wm_set_taskbar_ends(LAUNCH_W, TRAY_W,
                        launcher_paint, launcher_click,
                        tray_paint,     tray_click);
    wm_set_overlay(menu_paint, menu_click);

    app_about_open();      // a friendly first window rather than a bare desktop

    wm_run();

    console_init();        // hand the framebuffer back to the text console
}
