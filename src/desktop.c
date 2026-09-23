// src/desktop.c — the desktop shell: wallpaper, menu bar, dock and menus
//
// This is the policy layer that sits on top of the window manager. wm.c knows
// how to stack, drag, focus and paint windows but nothing about what the
// background looks like or what lives at the edges of the screen; everything
// specific to hawkOS's desktop lives here.
//
// The shape is the one every modern desktop settled on: a thin bar across the
// top carrying the system menus and the status items, and a floating dock at
// the bottom that is both the launcher and the list of what is running. It
// replaced a Windows-style taskbar with one button per window, which meant
// the only way to start an app was a menu nobody would find and the only
// thing the bar could tell you was what was already open.
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

// ---------------------------------------------------------------- metrics

#define ICON_S       48        // dock icon, at rest
#define ICON_GROW    16        // how much the icon under the pointer gains
#define ICON_REACH   84        // how far either side the magnification spreads
#define DOCK_GAP     14
#define DOCK_PAD     12
#define DOCK_BOTTOM  10
#define DOCK_H       (ICON_S + 2 * DOCK_PAD)

#define MENU_ROW     26
#define MENU_PAD     10

typedef void (*icon_open_t)(void);
typedef void (*icon_art_t)(int cx, int cy, int s);

typedef struct {
    const char* label;
    icon_art_t  art;
    icon_open_t open;
} app_t;

// ------------------------------------------------------------- pictograms
//
// Drawn from primitives rather than stored as bitmaps. At this size each one
// is a handful of shapes, and keeping them as code means they follow the
// palette and scale with the dock's magnification instead of needing a
// separate asset per size and per appearance.
//
// Every icon is a rounded tile of the same size with a mark inside it, which
// is what makes five icons drawn by five different functions read as one set.

static void tile(int cx, int cy, int s, uint32_t top, uint32_t bot){
    int x = cx - s / 2, y = cy - s / 2;
    uint32_t stops[2] = { top, bot };
    int r = s / 4;
    // The gradient goes down the tile and the rounded shape is punched out of
    // it by drawing the gradient one row at a time inside the corner insets.
    for (int row = 0; row < s; row++){
        uint32_t inset = 0;
        if (row < r)          inset = gfx_round_inset((uint32_t)r, (uint32_t)row);
        else if (row >= s - r) inset = gfx_round_inset((uint32_t)r, (uint32_t)(s - 1 - row));
        uint32_t a = stops[0], b = stops[1];
        uint32_t rr = (((a >> 16) & 0xFF) * (s - row) + ((b >> 16) & 0xFF) * row) / s;
        uint32_t gg = (((a >>  8) & 0xFF) * (s - row) + ((b >>  8) & 0xFF) * row) / s;
        uint32_t bb = ((a & 0xFF) * (s - row) + (b & 0xFF) * row) / s;
        gfx_fill_rect((uint32_t)(x + (int)inset), (uint32_t)(y + row),
                      (uint32_t)(s - 2 * (int)inset), 1, GFX_RGB(rr, gg, bb));
    }
}

// Files: the two-tone square with a face. Half light, half dark, two eyes and
// a smile — the most recognisable file-manager icon there is, and it happens
// to be four rectangles and a curve.
static void art_files(int cx, int cy, int s){
    tile(cx, cy, s, GFX_RGB(0x7F, 0xC7, 0xFF), GFX_RGB(0x2E, 0x8B, 0xE0));
    int x = cx - s / 2, y = cy - s / 2, r = s / 4;
    for (int row = 0; row < s; row++){
        uint32_t inset = 0;
        if (row < r)          inset = gfx_round_inset((uint32_t)r, (uint32_t)row);
        else if (row >= s - r) inset = gfx_round_inset((uint32_t)r, (uint32_t)(s - 1 - row));
        int lx = x + (int)inset;
        int half = cx - lx;
        if (half > 0) gfx_fill_rect((uint32_t)lx, (uint32_t)(y + row), (uint32_t)half, 1,
                                    GFX_RGB(0xEC, 0xF3, 0xFA));
    }
    // The face. Each feature is drawn in the colour of the half it does not
    // sit on, which is the trick that makes the two-tone square read as one
    // face rather than as two panels that happen to be adjacent.
    uint32_t ink = GFX_RGB(0x22, 0x33, 0x44), pap = GFX_RGB(0xF0, 0xF6, 0xFC);
    int ew = s / 14; if (ew < 2) ew = 2;
    int eh = s / 7;  if (eh < 4) eh = 4;
    int ey = cy - s / 5;
    gfx_fill_rect((uint32_t)(cx - s / 5 - ew / 2), (uint32_t)ey, (uint32_t)ew, (uint32_t)eh, ink);
    gfx_fill_rect((uint32_t)(cx + s / 6 - ew / 2), (uint32_t)ey, (uint32_t)ew, (uint32_t)eh, pap);

    int mw = s / 3, my = cy + s / 8;
    for (int i = -mw; i <= mw; i++){
        int lift = (i * i) / (mw * 4 > 0 ? mw * 4 : 1);
        gfx_fill_rect((uint32_t)(cx + i), (uint32_t)(my - lift), 1, 2, i < 0 ? ink : pap);
    }
}

static void art_term(int cx, int cy, int s){
    tile(cx, cy, s, GFX_RGB(0x3C, 0x3F, 0x46), GFX_RGB(0x15, 0x16, 0x1A));
    int px = cx - s / 4, py = cy - 6;
    for (int i = 0; i < 5; i++){
        gfx_fill_rect((uint32_t)(px + i), (uint32_t)(py + i), 2, 2, GFX_RGB(0x8B, 0xEF, 0xA8));
        gfx_fill_rect((uint32_t)(px + i), (uint32_t)(py + 8 - i), 2, 2, GFX_RGB(0x8B, 0xEF, 0xA8));
    }
    gfx_fill_rect((uint32_t)(cx), (uint32_t)(cy + 5), (uint32_t)(s / 4), 2,
                  GFX_RGB(0x8B, 0xEF, 0xA8));
}

static void art_taskman(int cx, int cy, int s){
    tile(cx, cy, s, GFX_RGB(0xFA, 0xFB, 0xFD), GFX_RGB(0xD8, 0xDC, 0xE4));
    static const int BARS[5] = { 7, 16, 11, 21, 13 };
    int bw = s / 9, base = cy + s / 4;
    for (int i = 0; i < 5; i++){
        int h = BARS[i] * s / 48;
        gfx_fill_rect((uint32_t)(cx - s / 3 + i * (bw + 2)), (uint32_t)(base - h),
                      (uint32_t)bw, (uint32_t)h,
                      i == 3 ? GFX_RGB(0x2E, 0x8B, 0xE0) : GFX_RGB(0x5A, 0x62, 0x70));
    }
}

// Browser: the compass. A blue disc with a two-tone needle across it, which
// is legible as "the internet" at 48 pixels in a way a globe is not.
static void art_browser(int cx, int cy, int s){
    int r = s / 2;
    gfx_fill_circle(cx, cy, r, GFX_RGB(0x2C, 0x8F, 0xE8));
    gfx_fill_circle(cx, cy, r - 3, GFX_RGB(0xE9, 0xF3, 0xFC));
    gfx_fill_circle(cx, cy, r - 5, GFX_RGB(0x1F, 0x7C, 0xD6));
    for (int i = 0; i < r - 6; i++){
        int w = (r - 6 - i) / 2 + 1;
        gfx_fill_rect((uint32_t)(cx + i - w / 2), (uint32_t)(cy - i), (uint32_t)w, 1,
                      GFX_RGB(0xFF, 0x5F, 0x57));
        gfx_fill_rect((uint32_t)(cx - i - w / 2), (uint32_t)(cy + i), (uint32_t)w, 1,
                      GFX_RGB(0xF4, 0xF7, 0xFB));
    }
}

static void art_about(int cx, int cy, int s){
    tile(cx, cy, s, GFX_RGB(0x6E, 0x76, 0x86), GFX_RGB(0x3A, 0x40, 0x4C));
    int w = s / 10; if (w < 2) w = 2;
    gfx_fill_rect((uint32_t)(cx - w / 2), (uint32_t)(cy - s / 4), (uint32_t)w, (uint32_t)w,
                  GFX_RGB(0xFF, 0xFF, 0xFF));
    gfx_fill_rect((uint32_t)(cx - w / 2), (uint32_t)(cy - s / 4 + w * 2), (uint32_t)w,
                  (uint32_t)(s / 2 - w * 2), GFX_RGB(0xFF, 0xFF, 0xFF));
}

static const app_t APPS[] = {
    { "Files",        art_files,   app_files_open   },
    { "Terminal",     art_term,    app_term_open    },
    { "Browser",      art_browser, app_browser_open },
    { "Activity",     art_taskman, app_taskman_open },
    { "About",        art_about,   app_about_open   },   // matches "About hawkOS"
};
#define APP_N ((int)(sizeof(APPS) / sizeof(APPS[0])))

// The mark at the left of the menu bar. A hawk's head in silhouette: a wedge
// for the beak and a filled crest, drawn small enough that it reads as a
// logo rather than as a drawing.
static void art_logo(int cx, int cy, uint32_t c){
    gfx_fill_circle(cx - 2, cy, 6, c);                       // head
    for (int i = 0; i < 6; i++)                              // beak, a wedge
        gfx_fill_rect((uint32_t)(cx + 3), (uint32_t)(cy - 2 + i / 2), (uint32_t)(6 - i), 1, c);
    for (int i = 0; i < 6; i++)                              // crest, swept back
        gfx_fill_rect((uint32_t)(cx - 8 - i / 2), (uint32_t)(cy - 6 + i), 3, 1, c);
}

// ---------------------------------------------------------------- state

static int   menu_open   = -1;       // index into MENUS, or -1
static int   dock_hover  = -1;
static char  clock_buf[32];

// Which window each dock slot maps to, rebuilt on every paint. A dock icon
// activates a running app rather than opening a second copy of it, and the
// only way to know an app is running is to match window titles against the
// app list -- there is no process/app registry to ask.
static uint32_t app_win[APP_N];

static const char* const MONTHS[13] = {
    "", "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

// Zeller's congruence. The RTC gives a date and no weekday, and a menu bar
// clock that cannot say what day it is only tells you half of what a clock
// is for.
static const char* weekday(int y, int m, int d){
    static const char* const W[7] = { "Sat","Sun","Mon","Tue","Wed","Thu","Fri" };
    if (m < 3){ m += 12; y -= 1; }
    int k = y % 100, j = y / 100;
    int h = (d + (13 * (m + 1)) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
    return W[h < 0 ? 0 : h];
}

// ------------------------------------------------------------- wallpaper

static void desktop_paint(void){
    uint32_t W = gfx_width(), H = gfx_height();
    gfx_mgradient(0, 0, W, H, th.wall, 8);
}

static void desktop_click(int mx, int my){
    (void)mx; (void)my;
    if (menu_open >= 0) menu_open = -1;
}

// -------------------------------------------------------------- dock

static int dock_geometry(int* x0, int* y0, int* w, int* h){
    uint32_t W = gfx_width(), H = gfx_height();
    int dw = APP_N * ICON_S + (APP_N - 1) * DOCK_GAP + 2 * DOCK_PAD;
    *w  = dw;
    *h  = DOCK_H;
    *x0 = ((int)W - dw) / 2;
    *y0 = (int)H - DOCK_BOTTOM - DOCK_H;
    return dw;
}

// Centre of dock slot `i`, at rest.
static int dock_slot_x(int x0, int i){
    return x0 + DOCK_PAD + i * (ICON_S + DOCK_GAP) + ICON_S / 2;
}

// How much slot `i` grows given where the pointer is. Linear falloff over
// ICON_REACH pixels: the icon under the cursor is largest and its neighbours
// taper, which is what makes the row feel like one surface being pushed
// rather than one icon changing size on its own.
static int dock_growth(int cx, int mouse_in, int mx){
    if (!mouse_in) return 0;
    int d = mx > cx ? mx - cx : cx - mx;
    if (d >= ICON_REACH) return 0;
    return (ICON_GROW * (ICON_REACH - d)) / ICON_REACH;
}

static void refresh_app_windows(void){
    wm_info_t info[WM_MAX_WINDOWS];
    int n = wm_snapshot(info, WM_MAX_WINDOWS);
    for (int a = 0; a < APP_N; a++){
        app_win[a] = 0;
        for (int i = 0; i < n; i++){
            // Match on the leading word of the title: an app renames its
            // window as it works ("Browser - Wikipedia"), and the dock still
            // has to recognise it as the same app.
            uint32_t l = strlen(APPS[a].label);
            if (kstrnicmp(info[i].title, APPS[a].label, l) == 0){ app_win[a] = info[i].id; break; }
        }
    }
}

static void dock_paint(int bx, int by, int bw, int bh){
    (void)bx; (void)by; (void)bw; (void)bh;

    int x0, y0, w, h;
    dock_geometry(&x0, &y0, &w, &h);

    int mx = mouse_x(), my = mouse_y();
    int mouse_in = (my >= y0 - ICON_GROW && mx >= x0 - 20 && mx < x0 + w + 20);
    dock_hover = -1;

    refresh_app_windows();

    // The panel itself: translucent, so the wallpaper reads through it and
    // the dock looks like it is floating on the desktop rather than cut out
    // of it. The hairline along the top is the only hard edge.
    gfx_blend_round_rect((uint32_t)x0, (uint32_t)y0, (uint32_t)w, (uint32_t)h, 18,
                         TH_DOCK, 205);
    gfx_draw_round_rect((uint32_t)x0, (uint32_t)y0, (uint32_t)w, (uint32_t)h, 18,
                        TH_DOCK_EDGE);

    for (int i = 0; i < APP_N; i++){
        int cx = dock_slot_x(x0, i);
        int grow = dock_growth(cx, mouse_in, mx);
        int s = ICON_S + grow;
        // Icons grow upwards out of the dock, keeping their feet on the same
        // line: growing about the centre would push them through the panel.
        int cy = y0 + DOCK_PAD + ICON_S / 2 - grow / 2;

        if (grow > ICON_GROW / 2) dock_hover = i;
        APPS[i].art(cx, cy, s);

        if (app_win[i])
            gfx_fill_circle(cx, y0 + h - 6, 2, TH_DOCK_DOT);
    }

    // The label of whatever the pointer is over, above the dock. macOS shows
    // it in a bubble; a plain line of text over the wallpaper reads the same
    // and does not need a second surface behind it.
    if (dock_hover >= 0){
        const char* l = APPS[dock_hover].label;
        int tw = (int)gfx_text_width(l);
        int lx = dock_slot_x(x0, dock_hover) - tw / 2;
        int ly = y0 - 26;
        gfx_blend_round_rect((uint32_t)(lx - 10), (uint32_t)(ly - 4),
                             (uint32_t)(tw + 20), 24, 8, TH_DOCK, 225);
        gfx_draw_round_rect((uint32_t)(lx - 10), (uint32_t)(ly - 4),
                            (uint32_t)(tw + 20), 24, 8, TH_DOCK_EDGE);
        gfx_text((uint32_t)lx, (uint32_t)ly, l, TH_TEXT, GFX_TRANSPARENT);
    }
}

static void activate(int i){
    if (i < 0 || i >= APP_N) return;
    // A running app is raised rather than opened again: five Terminal windows
    // is never what a click on one icon meant.
    if (app_win[i] && wm_focus_id(app_win[i]) == 0) return;
    if (APPS[i].open) APPS[i].open();
}

static int dock_click(int mx, int my){
    int x0, y0, w, h;
    dock_geometry(&x0, &y0, &w, &h);
    if (mx < x0 || mx >= x0 + w || my < y0 || my >= y0 + h) return 0;

    refresh_app_windows();
    for (int i = 0; i < APP_N; i++){
        int cx = dock_slot_x(x0, i);
        if (mx >= cx - ICON_S / 2 - DOCK_GAP / 2 && mx < cx + ICON_S / 2 + DOCK_GAP / 2){
            activate(i);
            return 1;
        }
    }
    return 1;
}

// --------------------------------------------------------------- menus

static void act_about(void){ app_about_open(); }
static void act_appearance(void){
    theme_set_dark(!theme_is_dark());
    // Windows repaint from the palette every frame and pick the change up on
    // their own. A rendered page does not: it was laid out with the colours
    // that were current at the time, and has to be built again.
    app_browser_relayout();
}
static void act_quit(void){ wm_quit(); }

static void act_files(void){ activate(0); }
static void act_term(void){ activate(1); }
static void act_browser(void){ activate(2); }
static void act_activity(void){ activate(3); }

typedef struct { const char* label; void (*run)(void); } item_t;

static const item_t M_SYSTEM[] = {
    { "About This System", act_about },
    { "-",                 0 },
    // Relabelled at draw time to say what it will do rather than what it is.
    { "Use Dark Appearance", act_appearance },
    { "-",                 0 },
    { "Quit to Console",   act_quit },
};

static const item_t M_APPS[] = {
    { "Files",    act_files },
    { "Terminal", act_term },
    { "Browser",  act_browser },
    { "Activity Monitor", act_activity },
};

typedef struct {
    const char*   title;
    const item_t* items;
    int           n;
    int           x, w;          // filled in at paint time
} menu_t;

static menu_t MENUS[] = {
    { "hawkOS",  M_SYSTEM, (int)(sizeof(M_SYSTEM)/sizeof(M_SYSTEM[0])), 0, 0 },
    { "Apps",    M_APPS,   (int)(sizeof(M_APPS)/sizeof(M_APPS[0])),     0, 0 },
    { "Window",  0,        0,                                           0, 0 },
};
#define MENU_N ((int)(sizeof(MENUS) / sizeof(MENUS[0])))

// The Window menu is built from whatever is open, so it cannot be a static
// table. Rebuilt whenever the menu bar is laid out.
static wm_info_t win_list[WM_MAX_WINDOWS];
static int       win_list_n = 0;

static int menu_item_count(int m){
    return (m == 2) ? (win_list_n ? win_list_n : 1) : MENUS[m].n;
}

static const char* menu_item_label(int m, int i){
    if (m == 0 && i == 2)
        return theme_is_dark() ? "Use Light Appearance" : "Use Dark Appearance";
    if (m != 2) return MENUS[m].items[i].label;
    if (!win_list_n) return "No Windows";
    return win_list[i].title;
}

static void menu_item_run(int m, int i){
    if (m != 2){ if (MENUS[m].items[i].run) MENUS[m].items[i].run(); return; }
    if (win_list_n) wm_focus_id(win_list[i].id);
}

// Lays the titles out along the bar and returns where the status area starts.
static void menu_layout(void){
    int x = 44;
    for (int m = 0; m < MENU_N; m++){
        int w = (int)gfx_text_width(MENUS[m].title) + 2 * MENU_PAD + 4;
        MENUS[m].x = x;
        MENUS[m].w = w;
        x += w;
    }
    win_list_n = wm_snapshot(win_list, WM_MAX_WINDOWS);
}

static void menubar_paint(int bx, int by, int bw, int bh){
    (void)bx; (void)by;
    uint32_t W = (uint32_t)bw;

    // Translucent, like the dock: the wallpaper's colour reads faintly
    // through it, which is what stops the bar from looking like a strip of
    // grey pasted over the top of the screen.
    gfx_blend_rect(0, 0, W, (uint32_t)bh, TH_MENUBAR, 214);
    gfx_hline(0, (uint32_t)(bh - 1), W, TH_MENUBAR_EDGE);

    menu_layout();
    art_logo(20, bh / 2, TH_MENUBAR_TEXT);

    for (int m = 0; m < MENU_N; m++){
        int on = (menu_open == m);
        if (on) gfx_fill_rect((uint32_t)MENUS[m].x, 0, (uint32_t)MENUS[m].w, (uint32_t)(bh - 1),
                              TH_ACCENT);
        gfx_text((uint32_t)(MENUS[m].x + MENU_PAD + 2), (uint32_t)((bh - 16) / 2),
                 MENUS[m].title, on ? TH_ACCENT_TEXT : TH_MENUBAR_TEXT, GFX_TRANSPARENT);
    }

    // ------------------------------------------------------ status items
    //
    // Right to left, so each one is placed against the screen edge and a
    // longer value pushes its neighbours left instead of colliding with them.
    rtc_time_t t;
    rtc_read(&t);
    ksnprintf(clock_buf, sizeof(clock_buf), "%s %u %s  %02u:%02u",
              weekday(t.year, t.month, t.day), t.day,
              MONTHS[t.month >= 1 && t.month <= 12 ? t.month : 0], t.hour, t.min);

    int rx = (int)W - 14 - (int)gfx_text_width(clock_buf);
    gfx_text((uint32_t)rx, (uint32_t)((bh - 16) / 2), clock_buf,
             TH_MENUBAR_TEXT, GFX_TRANSPARENT);

    // Memory, against the 128 MB budget this project is built to.
    char buf[32];
    uint32_t total_kb = pmm_total_kb(), used_kb = pmm_used_kb();
    uint32_t pct = total_kb ? (used_kb * 100u / total_kb) : 0;
    ksnprintf(buf, sizeof(buf), "%u%%", pct);
    rx -= 20 + (int)gfx_text_width(buf);
    gfx_text((uint32_t)rx, (uint32_t)((bh - 16) / 2), buf,
             pct > 85 ? TH_WARN : TH_MENUBAR_DIM, GFX_TRANSPARENT);

    // Network: three rising bars, filled as far as the link has got. Reading
    // "is it up" off a shape is faster than reading it off an address, and
    // the address is in the browser's status bar for when it matters.
    int level = net_configured() ? 3 : (net_link_up() ? 1 : 0);
    rx -= 26;
    for (int i = 0; i < 3; i++)
        gfx_fill_rect((uint32_t)(rx + i * 5), (uint32_t)(bh / 2 + 4 - (i + 1) * 3),
                      3, (uint32_t)((i + 1) * 3),
                      i < level ? TH_MENUBAR_TEXT : TH_MENUBAR_EDGE);
}

static int menubar_click(int mx, int my){
    (void)my;
    menu_layout();
    for (int m = 0; m < MENU_N; m++)
        if (mx >= MENUS[m].x && mx < MENUS[m].x + MENUS[m].w){
            menu_open = (menu_open == m) ? -1 : m;
            return 1;
        }
    menu_open = -1;
    return 1;
}

// A dropped menu, painted over everything by the window manager's overlay
// hook so it can extend past the bar and over any window.
static void menu_rect(int m, int* x, int* y, int* w, int* h){
    int rows = menu_item_count(m);
    int wide = 150;
    for (int i = 0; i < rows; i++){
        int tw = (int)gfx_text_width(menu_item_label(m, i)) + 44;
        if (tw > wide) wide = tw;
    }
    *x = MENUS[m].x;
    *y = WM_MENUBAR_H + 2;
    *w = wide;
    *h = rows * MENU_ROW + 12;
}

static void overlay_paint(void){
    if (menu_open < 0) return;
    int m = menu_open, x, y, w, h;
    menu_rect(m, &x, &y, &w, &h);

    gfx_shadow(x, y, w, h, 10, TH_WIN_SHADOW);
    gfx_fill_round_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, 10, TH_PANEL);
    gfx_draw_round_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, 10, TH_PANEL_EDGE);

    int mx = mouse_x(), my = mouse_y();
    for (int i = 0; i < menu_item_count(m); i++){
        int ry = y + 6 + i * MENU_ROW;
        const char* label = menu_item_label(m, i);

        if (label[0] == '-' && label[1] == 0){
            gfx_hline((uint32_t)(x + 12), (uint32_t)(ry + MENU_ROW / 2),
                      (uint32_t)(w - 24), TH_PANEL_EDGE);
            continue;
        }

        int hot = (mx >= x && mx < x + w && my >= ry && my < ry + MENU_ROW);
        if (hot)
            gfx_fill_round_rect((uint32_t)(x + 5), (uint32_t)ry, (uint32_t)(w - 10),
                                MENU_ROW, 6, TH_ACCENT);

        gfx_text((uint32_t)(x + 18), (uint32_t)(ry + (MENU_ROW - 16) / 2), label,
                 hot ? TH_ACCENT_TEXT : TH_TEXT, GFX_TRANSPARENT);
    }
}

static int overlay_click(int mx, int my){
    if (menu_open < 0) return 0;
    int m = menu_open, x, y, w, h;
    menu_rect(m, &x, &y, &w, &h);

    // A click inside the bar is the bar's business -- it has to be able to
    // switch between menus while one is open.
    if (my < WM_MENUBAR_H) return 0;

    if (mx < x || mx >= x + w || my < y || my >= y + h){
        // Outside: dismiss, and swallow the click so it does not also
        // activate whatever it landed on.
        menu_open = -1;
        return 1;
    }

    for (int i = 0; i < menu_item_count(m); i++){
        int ry = y + 6 + i * MENU_ROW;
        if (my >= ry && my < ry + MENU_ROW){
            const char* label = menu_item_label(m, i);
            menu_open = -1;
            if (!(label[0] == '-' && label[1] == 0)) menu_item_run(m, i);
            return 1;
        }
    }
    menu_open = -1;
    return 1;
}

static int desktop_key(int key){
    if (key == 27 && menu_open >= 0){ menu_open = -1; return 1; }
    if (key == KEY_F1){ menu_open = (menu_open == 1) ? -1 : 1; return 1; }
    return 0;
}

// --------------------------------------------------------------------- run

void desktop_run(void){
    if (!gfx_available()) return;

    theme_init();
    wm_init();
    wm_set_root(desktop_paint, desktop_click, desktop_key);
    wm_set_chrome(menubar_paint, menubar_click, dock_paint, dock_click);
    wm_set_overlay(overlay_paint, overlay_click);

    app_about_open();      // a friendly first window rather than a bare desktop

    wm_run();

    console_init();        // hand the framebuffer back to the text console
}
