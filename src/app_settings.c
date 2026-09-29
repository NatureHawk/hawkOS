// src/app_settings.c — Settings
//
// Every control here does the same three things: change a field of `settings`,
// apply it to the running system, and write it to disk. There is no Apply
// button and no separate "unsaved" state, because a preference that needs a
// second click to take effect is a preference people set, see nothing happen,
// and set again.
#include <stdint.h>
#include "header/apps.h"
#include "header/wm.h"
#include "header/theme.h"
#include "header/gfx.h"
#include "header/kheap.h"
#include "header/kstring.h"
#include "header/settings.h"
#include "header/font.h"

#define PAD       24
#define CARD_W    132
#define CARD_H    84
#define SWATCH_W  100
#define SWATCH_H  60
#define GAP       16

typedef struct { int dummy; } settings_win_t;

// ---------------------------------------------------------------- layout
//
// Rectangles are computed in one place so that painting and hit-testing cannot
// drift apart, the way they did in the first Files window.

typedef struct { int x, y, w, h; } rect_t;

static int inside(rect_t r, int x, int y){
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static rect_t card_rect(int i){ return (rect_t){ PAD + i * (CARD_W + GAP), 50, CARD_W, CARD_H }; }
static rect_t wall_rect(int i){ return (rect_t){ PAD + i * (SWATCH_W + GAP), 214, SWATCH_W, SWATCH_H }; }
static rect_t clock_rect(int i){ return (rect_t){ PAD + i * 112, 344, 104, 28 }; }
static rect_t zoom_rect(void){ return (rect_t){ PAD, 414, 40, 22 }; }

// ---------------------------------------------------------------- painting

static void heading(int x, int y, const char* s){
    gfx_text((uint32_t)x, (uint32_t)y, s, TH_TEXT, GFX_TRANSPARENT);
}

static void ring(rect_t r, int on){
    if (on){
        gfx_draw_round_rect((uint32_t)(r.x - 3), (uint32_t)(r.y - 3),
                            (uint32_t)(r.w + 6), (uint32_t)(r.h + 6), 9, TH_ACCENT);
        gfx_draw_round_rect((uint32_t)(r.x - 2), (uint32_t)(r.y - 2),
                            (uint32_t)(r.w + 4), (uint32_t)(r.h + 4), 8, TH_ACCENT);
    }
}

// A miniature window in one appearance, drawn from fixed colours so both
// cards show what they will look like whichever appearance is active now.
static void paint_card(rect_t r, int dark, int on){
    uint32_t bg  = dark ? GFX_RGB(0x1E, 0x1E, 0x21) : GFX_RGB(0xFF, 0xFF, 0xFF);
    uint32_t bar = dark ? GFX_RGB(0x2C, 0x2C, 0x31) : GFX_RGB(0xEC, 0xEC, 0xEF);
    uint32_t ink = dark ? GFX_RGB(0x6A, 0x6D, 0x75) : GFX_RGB(0xB8, 0xBB, 0xC2);
    uint32_t edge = dark ? GFX_RGB(0x3A, 0x3B, 0x41) : GFX_RGB(0xC2, 0xC4, 0xCA);

    ring(r, on);
    gfx_fill_round_rect((uint32_t)r.x, (uint32_t)r.y, (uint32_t)r.w, (uint32_t)r.h, 8, bg);
    gfx_fill_round_rect((uint32_t)r.x, (uint32_t)r.y, (uint32_t)r.w, 18, 8, bar);
    gfx_fill_rect((uint32_t)r.x, (uint32_t)(r.y + 9), (uint32_t)r.w, 9, bar);
    gfx_fill_circle(r.x + r.w - 12, r.y + 9, 3, TH_LIGHT_CLOSE);
    gfx_fill_circle(r.x + r.w - 24, r.y + 9, 3, TH_LIGHT_MAX);
    gfx_fill_circle(r.x + r.w - 36, r.y + 9, 3, TH_LIGHT_MIN);
    gfx_fill_rect((uint32_t)(r.x + 12), (uint32_t)(r.y + 30), (uint32_t)(r.w - 40), 5, ink);
    gfx_fill_rect((uint32_t)(r.x + 12), (uint32_t)(r.y + 42), (uint32_t)(r.w - 60), 5, ink);
    gfx_fill_rect((uint32_t)(r.x + 12), (uint32_t)(r.y + 54), (uint32_t)(r.w - 24), 5, ink);
    gfx_fill_round_rect((uint32_t)(r.x + 12), (uint32_t)(r.y + 66), 34, 10, 4, TH_ACCENT);
    gfx_draw_round_rect((uint32_t)r.x, (uint32_t)r.y, (uint32_t)r.w, (uint32_t)r.h, 8, edge);
}

static void paint_wall(rect_t r, int n, int on){
    uint32_t stops[8];
    for (int i = 0; i < 8; i++) stops[i] = theme_wallpaper_stop(n, theme_is_dark(), i);
    ring(r, on);
    gfx_mgradient((uint32_t)r.x, (uint32_t)r.y, (uint32_t)r.w, (uint32_t)r.h, stops, 8);
    // The corners of a gradient rectangle are square; a hairline in the
    // window colour rounds them off visually without a masked fill.
    gfx_draw_round_rect((uint32_t)r.x, (uint32_t)r.y, (uint32_t)r.w, (uint32_t)r.h, 8,
                        TH_PANEL_EDGE);
}

static void paint_segment(rect_t r, const char* label, int on){
    gfx_fill_round_rect((uint32_t)r.x, (uint32_t)r.y, (uint32_t)r.w, (uint32_t)r.h, 7,
                        on ? TH_ACCENT : TH_CONTROL);
    if (!on)
        gfx_draw_round_rect((uint32_t)r.x, (uint32_t)r.y, (uint32_t)r.w, (uint32_t)r.h, 7,
                            TH_CONTROL_EDGE);
    int tw = (int)gfx_text_width(label);
    gfx_text((uint32_t)(r.x + (r.w - tw) / 2), (uint32_t)(r.y + (r.h - 16) / 2), label,
             on ? TH_ACCENT_TEXT : TH_TEXT, GFX_TRANSPARENT);
}

static void paint_toggle(rect_t r, int on){
    gfx_fill_round_rect((uint32_t)r.x, (uint32_t)r.y, (uint32_t)r.w, (uint32_t)r.h, 11,
                        on ? TH_OK : TH_CONTROL_EDGE);
    int kx = on ? r.x + r.w - r.h + 2 : r.x + 2;
    gfx_fill_circle(kx + (r.h - 4) / 2, r.y + r.h / 2, (r.h - 4) / 2, GFX_RGB(0xFF, 0xFF, 0xFF));
}

static void paint(wm_window_t* win){
    int x, y, w, h;
    wm_client_rect(win, &x, &y, &w, &h);

    gfx_fill_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, TH_WIN_BG);

    // Everything below is laid out in client coordinates. Drawing through a
    // translated origin would need a transform in gfx; adding x and y to each
    // rectangle as it is drawn is what the other apps do.
    #define OFF(r) ((rect_t){ x + (r).x, y + (r).y, (r).w, (r).h })

    heading(x + PAD, y + 20, "Appearance");
    for (int i = 0; i < 2; i++){
        rect_t r = OFF(card_rect(i));
        paint_card(r, i, theme_is_dark() == i);
        const char* label = i ? "Dark" : "Light";
        int tw = (int)gfx_text_width(label);
        gfx_text((uint32_t)(r.x + (r.w - tw) / 2), (uint32_t)(r.y + r.h + 10), label,
                 TH_TEXT_MUTED, GFX_TRANSPARENT);
    }

    heading(x + PAD, y + 182, "Wallpaper");
    int nw = theme_wallpaper_count();
    for (int i = 0; i < nw; i++){
        rect_t r = OFF(wall_rect(i));
        paint_wall(r, i, settings.wallpaper == i);
        const char* label = theme_wallpaper_name(i);
        int tw = (int)gfx_text_width(label);
        gfx_text((uint32_t)(r.x + (r.w - tw) / 2), (uint32_t)(r.y + r.h + 10), label,
                 TH_TEXT_MUTED, GFX_TRANSPARENT);
    }

    heading(x + PAD, y + 312, "Menu bar clock");
    paint_segment(OFF(clock_rect(0)), "24-hour", settings.clock24);
    paint_segment(OFF(clock_rect(1)), "12-hour", !settings.clock24);

    heading(x + PAD, y + 384, "Dock");
    rect_t z = OFF(zoom_rect());
    paint_toggle(z, settings.dock_zoom);
    gfx_text((uint32_t)(z.x + z.w + 12), (uint32_t)(z.y + 3), "Magnify icons under the pointer",
             TH_TEXT, GFX_TRANSPARENT);

    #undef OFF

    const char* note = settings_persistent()
        ? "Changes are saved to HAWKOS.CFG and kept across restarts."
        : "The disk is missing or read-only: changes last until restart.";
    gfx_text((uint32_t)(x + PAD), (uint32_t)(y + h - 26), note,
             settings_persistent() ? TH_TEXT_MUTED : TH_WARN, GFX_TRANSPARENT);
}

// ------------------------------------------------------------------ input

static void click(int cx, int cy){
    for (int i = 0; i < 2; i++)
        if (inside(card_rect(i), cx, cy)){
            settings.dark = i;
            settings_commit();
            return;
        }
    for (int i = 0; i < theme_wallpaper_count(); i++)
        if (inside(wall_rect(i), cx, cy)){
            settings.wallpaper = i;
            settings_commit();
            return;
        }
    for (int i = 0; i < 2; i++)
        if (inside(clock_rect(i), cx, cy)){
            settings.clock24 = (i == 0);
            settings_commit();
            return;
        }
    rect_t z = zoom_rect();
    // The label beside the switch is part of the target, as it is everywhere.
    if (cy >= z.y && cy < z.y + z.h && cx >= z.x && cx < z.x + 300){
        settings.dock_zoom = !settings.dock_zoom;
        settings_commit();
    }
}

static void handler(wm_window_t* win, const wm_event_t* ev){
    settings_win_t* s = (settings_win_t*)win->user;
    if (!s) return;

    switch (ev->type){
        case WM_EV_PAINT:      paint(win); break;
        case WM_EV_MOUSE_DOWN: click(ev->x, ev->y); wm_invalidate(); break;
        case WM_EV_CLOSE:
            kfree(s);
            win->user = 0;
            break;
        default: break;
    }
}

void app_settings_open(void){
    settings_win_t* s = (settings_win_t*)kmalloc(sizeof(*s));
    if (!s) return;
    s->dummy = 0;
    wm_open("Settings", 200, 60, 540, 540, handler, s);
}
