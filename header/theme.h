#pragma once
#include <stdint.h>
#include "header/gfx.h"

// One palette for the whole system, chosen at runtime rather than compiled in.
//
// It used to be a block of #defines, which meant the system had exactly one
// look and every app baked it in at compile time. Appearance is a setting on
// the machines this is modelled on, so the names below now read fields out of
// a struct that theme_set_dark() swaps. Nothing else had to change: every
// TH_* spelling an app already used still works, and still costs one load.
//
// Contrast: the light palette puts body text on its background at about
// 13:1 and muted text at 4.9:1; the dark palette lands at 12:1 and 5.1:1.
// Both clear the 4.5:1 that small text needs.

typedef struct {
    // Wallpaper. Eight stops down the screen rather than two: a single
    // gradient from one colour to another reads as a default, and stops are
    // free because the fill is one solid row per scanline either way.
    uint32_t wall[8];

    // Desktop furniture
    uint32_t desk_top, desk_bot;
    uint32_t menubar, menubar_edge, menubar_text, menubar_dim;
    uint32_t dock, dock_edge, dock_dot;

    // Window chrome
    uint32_t win_bg, win_edge, win_shadow;
    uint32_t title_on, title_off, title_text_on, title_text_off;
    uint32_t light_close, light_min, light_max, light_off;

    // Panels inside windows: headers, toolbars, sidebars, status bars
    uint32_t panel, panel_edge, sidebar, field, control, control_edge;

    // Text
    uint32_t text, text_muted, text_dim, text_inverse;

    // Accents and states
    uint32_t accent, accent_dim, accent_text, select, select_text, hover;
    uint32_t link, ok, warn, error, close;

    // Rendered web pages. A page is re-coloured into the system's appearance
    // rather than rendered on its own background: a browser that punched a
    // bright white rectangle into a dark desktop would be the one thing on
    // screen nobody could look at, and the same is true in reverse.
    uint32_t page_bg, page_text, page_head, page_muted, page_rule;
    uint32_t page_panel, page_code, page_frame, page_frame_bg;
    uint32_t page_field, page_field_bg, page_button, page_button_bg;
} th_palette_t;

extern th_palette_t th;

void theme_init(void);
void theme_set_dark(int on);
int  theme_is_dark(void);

// Pulls a colour a page asked for towards something legible on the current
// background: dark text is lifted in the dark appearance, light text is
// dropped in the light one. Hue is preserved. Without this a page that names
// its own near-background colour renders as a blank rectangle.
uint32_t th_readable(uint32_t c);

// Desktop
#define TH_DESK_TOP      (th.desk_top)
#define TH_DESK_BOT      (th.desk_bot)
#define TH_MENUBAR       (th.menubar)
#define TH_MENUBAR_EDGE  (th.menubar_edge)
#define TH_MENUBAR_TEXT  (th.menubar_text)
#define TH_MENUBAR_DIM   (th.menubar_dim)
#define TH_DOCK          (th.dock)
#define TH_DOCK_EDGE     (th.dock_edge)
#define TH_DOCK_DOT      (th.dock_dot)

// Retained so the older spelling in wm.h keeps resolving.
#define TH_TASKBAR       (th.dock)
#define TH_TASKBAR_EDGE  (th.dock_edge)

// Window chrome
#define TH_WIN_BG        (th.win_bg)
#define TH_WIN_EDGE      (th.win_edge)
#define TH_WIN_SHADOW    (th.win_shadow)
#define TH_TITLE_ON      (th.title_on)
#define TH_TITLE_OFF     (th.title_off)
#define TH_TITLE_TEXT_ON  (th.title_text_on)
#define TH_TITLE_TEXT_OFF (th.title_text_off)
#define TH_LIGHT_CLOSE   (th.light_close)
#define TH_LIGHT_MIN     (th.light_min)
#define TH_LIGHT_MAX     (th.light_max)
#define TH_LIGHT_OFF     (th.light_off)

// Panels
#define TH_PANEL         (th.panel)
#define TH_PANEL_EDGE    (th.panel_edge)
#define TH_SIDEBAR       (th.sidebar)
#define TH_FIELD         (th.field)
#define TH_CONTROL       (th.control)
#define TH_CONTROL_EDGE  (th.control_edge)

// Text
#define TH_TEXT          (th.text)
#define TH_TEXT_MUTED    (th.text_muted)
#define TH_TEXT_DIM      (th.text_dim)
#define TH_TEXT_INVERSE  (th.text_inverse)

// Accents and states
#define TH_ACCENT        (th.accent)
#define TH_ACCENT_DIM    (th.accent_dim)
#define TH_ACCENT_TEXT   (th.accent_text)
#define TH_SELECT        (th.select)
#define TH_SELECT_TEXT   (th.select_text)
#define TH_HOVER         (th.hover)
#define TH_LINK          (th.link)
#define TH_OK            (th.ok)
#define TH_WARN          (th.warn)
#define TH_ERROR         (th.error)
#define TH_CLOSE         (th.close)

// Rendered pages
#define TH_PAGE_BG        (th.page_bg)
#define TH_PAGE_TEXT      (th.page_text)
#define TH_PAGE_HEAD      (th.page_head)
#define TH_PAGE_MUTED     (th.page_muted)
#define TH_PAGE_RULE      (th.page_rule)
#define TH_PAGE_PANEL     (th.page_panel)
#define TH_PAGE_CODE      (th.page_code)
#define TH_PAGE_FRAME     (th.page_frame)
#define TH_PAGE_FRAME_BG  (th.page_frame_bg)
#define TH_PAGE_FIELD     (th.page_field)
#define TH_PAGE_FIELD_BG  (th.page_field_bg)
#define TH_PAGE_BUTTON    (th.page_button)
#define TH_PAGE_BUTTON_BG (th.page_button_bg)
