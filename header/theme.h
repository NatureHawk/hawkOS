#pragma once
#include "header/gfx.h"

// One dark palette for the whole system. Every window, app and rendered web
// page pulls its colours from here, so nothing anywhere in the UI paints a
// white rectangle. Values are chosen for contrast at 8x16 pixel text: the
// body/background pair sits around 11:1, well past the 4.5:1 that small text
// needs to stay comfortable.

// Desktop
#define TH_DESK_TOP      GFX_RGB(0x0A, 0x0E, 0x14)
#define TH_DESK_BOT      GFX_RGB(0x16, 0x20, 0x2B)
#define TH_TASKBAR       GFX_RGB(0x08, 0x0B, 0x10)
#define TH_TASKBAR_EDGE  GFX_RGB(0x25, 0x30, 0x3E)

// Window chrome
#define TH_WIN_BG        GFX_RGB(0x15, 0x1A, 0x21)
#define TH_WIN_EDGE      GFX_RGB(0x2A, 0x35, 0x43)
#define TH_TITLE_ON      GFX_RGB(0x1D, 0x28, 0x36)
#define TH_TITLE_OFF     GFX_RGB(0x13, 0x18, 0x1F)

// Panels inside windows: headers, toolbars, status bars
#define TH_PANEL         GFX_RGB(0x1A, 0x21, 0x2A)
#define TH_PANEL_EDGE    GFX_RGB(0x28, 0x32, 0x3F)
#define TH_FIELD         GFX_RGB(0x0E, 0x12, 0x18)

// Text
#define TH_TEXT          GFX_RGB(0xE2, 0xE8, 0xF0)
#define TH_TEXT_MUTED    GFX_RGB(0x8A, 0x97, 0xA7)
#define TH_TEXT_DIM      GFX_RGB(0x55, 0x60, 0x6E)

// Accents and states
#define TH_ACCENT        GFX_RGB(0xF5, 0xC5, 0x42)
#define TH_ACCENT_DIM    GFX_RGB(0x8A, 0x6E, 0x22)
#define TH_SELECT        GFX_RGB(0x22, 0x3B, 0x54)
#define TH_HOVER         GFX_RGB(0x1F, 0x2A, 0x37)
#define TH_LINK          GFX_RGB(0x6F, 0xB5, 0xF0)
#define TH_OK            GFX_RGB(0x4C, 0xC3, 0x8A)
#define TH_WARN          GFX_RGB(0xE0, 0xA0, 0x36)
#define TH_ERROR         GFX_RGB(0xE0, 0x60, 0x60)
#define TH_CLOSE         GFX_RGB(0xC0, 0x39, 0x2B)
