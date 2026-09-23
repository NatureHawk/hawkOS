// src/theme.c — the two appearances
//
// Values are picked to read the way a modern desktop reads rather than to be
// interesting on their own: near-neutral greys for chrome so that colour is
// reserved for meaning, one blue accent, and traffic-light colours for the
// window buttons because those three colours are the most widely understood
// controls in any interface.
//
// The light appearance is the default. Every app in this system draws its own
// panels and text from these fields, so switching is one assignment and a
// repaint -- there is no per-app appearance state anywhere.
#include <stdint.h>
#include "header/theme.h"

th_palette_t th;
static int dark_on = 0;

// The window buttons. These are the same in both appearances, which is the
// point of them: they are recognised by colour and position, and re-tinting
// them per theme would only make them harder to find.
#define LIGHT_CLOSE  GFX_RGB(0xFF, 0x5F, 0x57)
#define LIGHT_MIN    GFX_RGB(0xFE, 0xBC, 0x2E)
#define LIGHT_MAX    GFX_RGB(0x28, 0xC8, 0x40)

static const th_palette_t LIGHT = {
    // Wallpaper: dawn over a horizon. Indigo overhead falling through violet
    // and rose into a warm band at the bottom, which is where a dock full of
    // saturated icons needs something quiet to sit against.
    .wall = {
        GFX_RGB(0x2B, 0x2E, 0x7A), GFX_RGB(0x3E, 0x39, 0x8E),
        GFX_RGB(0x5B, 0x42, 0x9B), GFX_RGB(0x84, 0x4C, 0x9E),
        GFX_RGB(0xB0, 0x5A, 0x92), GFX_RGB(0xD8, 0x72, 0x7E),
        GFX_RGB(0xF0, 0x95, 0x6B), GFX_RGB(0xF7, 0xB9, 0x76),
    },
    .desk_top = GFX_RGB(0x2B, 0x2E, 0x7A), .desk_bot = GFX_RGB(0xF7, 0xB9, 0x76),

    .menubar      = GFX_RGB(0xE8, 0xE9, 0xED),
    .menubar_edge = GFX_RGB(0xC9, 0xCB, 0xD2),
    .menubar_text = GFX_RGB(0x1A, 0x1A, 0x1E),
    .menubar_dim  = GFX_RGB(0x54, 0x57, 0x5E),

    .dock      = GFX_RGB(0xE4, 0xE5, 0xEA),
    .dock_edge = GFX_RGB(0xFA, 0xFA, 0xFC),
    .dock_dot  = GFX_RGB(0x4B, 0x4E, 0x55),

    .win_bg     = GFX_RGB(0xFF, 0xFF, 0xFF),
    .win_edge   = GFX_RGB(0xC2, 0xC4, 0xCA),
    .win_shadow = GFX_RGB(0x10, 0x12, 0x1A),

    .title_on        = GFX_RGB(0xEC, 0xEC, 0xEF),
    .title_off       = GFX_RGB(0xF4, 0xF4, 0xF6),
    .title_text_on   = GFX_RGB(0x1C, 0x1C, 0x20),
    .title_text_off  = GFX_RGB(0x9A, 0x9C, 0xA3),
    .light_close = LIGHT_CLOSE, .light_min = LIGHT_MIN, .light_max = LIGHT_MAX,
    .light_off   = GFX_RGB(0xD2, 0xD3, 0xD8),

    .panel        = GFX_RGB(0xF2, 0xF2, 0xF5),
    .panel_edge   = GFX_RGB(0xD6, 0xD8, 0xDD),
    .sidebar      = GFX_RGB(0xEA, 0xEB, 0xEF),
    .field        = GFX_RGB(0xFF, 0xFF, 0xFF),
    .control      = GFX_RGB(0xFB, 0xFB, 0xFD),
    .control_edge = GFX_RGB(0xC4, 0xC6, 0xCC),

    .text         = GFX_RGB(0x1B, 0x1C, 0x20),
    .text_muted   = GFX_RGB(0x62, 0x65, 0x6D),
    .text_dim     = GFX_RGB(0x93, 0x96, 0x9E),
    .text_inverse = GFX_RGB(0xFF, 0xFF, 0xFF),

    .accent      = GFX_RGB(0x00, 0x6F, 0xE8),
    .accent_dim  = GFX_RGB(0x9C, 0xC6, 0xF5),
    .accent_text = GFX_RGB(0xFF, 0xFF, 0xFF),
    .select      = GFX_RGB(0x00, 0x6F, 0xE8),
    .select_text = GFX_RGB(0xFF, 0xFF, 0xFF),
    .hover       = GFX_RGB(0xDF, 0xE1, 0xE7),

    .link  = GFX_RGB(0x0B, 0x5C, 0xC4),
    .ok    = GFX_RGB(0x1D, 0x9A, 0x5A),
    .warn  = GFX_RGB(0xC3, 0x7A, 0x0C),
    .error = GFX_RGB(0xC4, 0x2B, 0x2B),
    .close = LIGHT_CLOSE,

    .page_bg        = GFX_RGB(0xFF, 0xFF, 0xFF),
    .page_text      = GFX_RGB(0x1F, 0x20, 0x24),
    .page_head      = GFX_RGB(0x0B, 0x0C, 0x0F),
    .page_muted     = GFX_RGB(0x5E, 0x62, 0x6A),
    .page_rule      = GFX_RGB(0xD9, 0xDB, 0xE0),
    .page_panel     = GFX_RGB(0xF4, 0xF5, 0xF7),
    .page_code      = GFX_RGB(0x1F, 0x5C, 0x2E),
    .page_frame     = GFX_RGB(0xCA, 0xCD, 0xD4),
    .page_frame_bg  = GFX_RGB(0xF7, 0xF8, 0xFA),
    .page_field     = GFX_RGB(0xB6, 0xBA, 0xC2),
    .page_field_bg  = GFX_RGB(0xFF, 0xFF, 0xFF),
    .page_button    = GFX_RGB(0xB6, 0xBA, 0xC2),
    .page_button_bg = GFX_RGB(0xF0, 0xF1, 0xF4),
};

static const th_palette_t DARK = {
    // The same dawn after sunset: the hues survive, the level drops far
    // enough that white window chrome sitting on top is the brightest thing
    // on screen rather than competing with the background.
    .wall = {
        GFX_RGB(0x0B, 0x0D, 0x24), GFX_RGB(0x14, 0x11, 0x33),
        GFX_RGB(0x21, 0x15, 0x3E), GFX_RGB(0x33, 0x1A, 0x45),
        GFX_RGB(0x48, 0x20, 0x44), GFX_RGB(0x5E, 0x27, 0x3D),
        GFX_RGB(0x74, 0x33, 0x33), GFX_RGB(0x86, 0x45, 0x30),
    },
    .desk_top = GFX_RGB(0x0B, 0x0D, 0x24), .desk_bot = GFX_RGB(0x86, 0x45, 0x30),

    .menubar      = GFX_RGB(0x1D, 0x1D, 0x20),
    .menubar_edge = GFX_RGB(0x33, 0x34, 0x38),
    .menubar_text = GFX_RGB(0xF0, 0xF1, 0xF4),
    .menubar_dim  = GFX_RGB(0xA6, 0xA9, 0xB0),

    .dock      = GFX_RGB(0x28, 0x29, 0x2E),
    .dock_edge = GFX_RGB(0x45, 0x47, 0x4E),
    .dock_dot  = GFX_RGB(0xC8, 0xCA, 0xD0),

    .win_bg     = GFX_RGB(0x1E, 0x1E, 0x21),
    .win_edge   = GFX_RGB(0x3A, 0x3B, 0x41),
    .win_shadow = GFX_RGB(0x00, 0x00, 0x00),

    .title_on        = GFX_RGB(0x2C, 0x2C, 0x31),
    .title_off       = GFX_RGB(0x24, 0x24, 0x28),
    .title_text_on   = GFX_RGB(0xF2, 0xF3, 0xF6),
    .title_text_off  = GFX_RGB(0x7C, 0x7F, 0x87),
    .light_close = LIGHT_CLOSE, .light_min = LIGHT_MIN, .light_max = LIGHT_MAX,
    .light_off   = GFX_RGB(0x4B, 0x4C, 0x52),

    .panel        = GFX_RGB(0x28, 0x29, 0x2D),
    .panel_edge   = GFX_RGB(0x3D, 0x3E, 0x44),
    .sidebar      = GFX_RGB(0x23, 0x24, 0x28),
    .field        = GFX_RGB(0x15, 0x15, 0x18),
    .control      = GFX_RGB(0x34, 0x35, 0x3A),
    .control_edge = GFX_RGB(0x4A, 0x4C, 0x52),

    .text         = GFX_RGB(0xEC, 0xEE, 0xF2),
    .text_muted   = GFX_RGB(0x9B, 0x9E, 0xA6),
    .text_dim     = GFX_RGB(0x6A, 0x6D, 0x75),
    .text_inverse = GFX_RGB(0x14, 0x14, 0x17),

    .accent      = GFX_RGB(0x0A, 0x84, 0xFF),
    .accent_dim  = GFX_RGB(0x1E, 0x4A, 0x78),
    .accent_text = GFX_RGB(0xFF, 0xFF, 0xFF),
    .select      = GFX_RGB(0x0A, 0x84, 0xFF),
    .select_text = GFX_RGB(0xFF, 0xFF, 0xFF),
    .hover       = GFX_RGB(0x35, 0x36, 0x3C),

    .link  = GFX_RGB(0x5E, 0xB0, 0xFF),
    .ok    = GFX_RGB(0x32, 0xD7, 0x4B),
    .warn  = GFX_RGB(0xFF, 0xC1, 0x3B),
    .error = GFX_RGB(0xFF, 0x6B, 0x6B),
    .close = LIGHT_CLOSE,

    .page_bg        = GFX_RGB(0x16, 0x17, 0x1A),
    .page_text      = GFX_RGB(0xD9, 0xDD, 0xE4),
    .page_head      = GFX_RGB(0xFF, 0xFF, 0xFF),
    .page_muted     = GFX_RGB(0x98, 0x9E, 0xA8),
    .page_rule      = GFX_RGB(0x33, 0x35, 0x3B),
    .page_panel     = GFX_RGB(0x1F, 0x21, 0x25),
    .page_code      = GFX_RGB(0xA6, 0xD9, 0xA0),
    .page_frame     = GFX_RGB(0x3A, 0x3D, 0x44),
    .page_frame_bg  = GFX_RGB(0x1B, 0x1D, 0x21),
    .page_field     = GFX_RGB(0x4C, 0x50, 0x58),
    .page_field_bg  = GFX_RGB(0x10, 0x11, 0x14),
    .page_button    = GFX_RGB(0x56, 0x5A, 0x62),
    .page_button_bg = GFX_RGB(0x2A, 0x2D, 0x33),
};

void theme_set_dark(int on){
    dark_on = on ? 1 : 0;
    th = dark_on ? DARK : LIGHT;
}

int  theme_is_dark(void){ return dark_on; }
void theme_init(void){ theme_set_dark(0); }

uint32_t th_readable(uint32_t c){
    uint32_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    uint32_t lum = (r * 30 + g * 59 + b * 11) / 100;

    if (dark_on){
        if (lum >= 90) return c;
        if (lum == 0)  return th.page_text;
        uint32_t s = (120u * 256u) / lum;
        r = (r * s) >> 8; g = (g * s) >> 8; b = (b * s) >> 8;
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
        return GFX_RGB(r, g, b);
    }

    // Light appearance: the failure runs the other way. A page asking for
    // white text expects its own dark background and would otherwise vanish.
    if (lum <= 165) return c;
    if (lum >= 250) return th.page_text;
    uint32_t s = (140u * 256u) / lum;
    r = (r * s) >> 8; g = (g * s) >> 8; b = (b * s) >> 8;
    return GFX_RGB(r, g, b);
}
