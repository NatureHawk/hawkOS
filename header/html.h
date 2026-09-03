#pragma once
#include <stdint.h>
#include "header/http.h"

// A laid-out page: a flat display list, not a DOM.
//
// Nothing here needs a tree. The layout pass walks the markup once, keeping
// a small style stack, and emits positioned text runs as it goes; painting
// is then a straight loop over runs that intersect the viewport, and
// scrolling is a single offset. That is what makes a browser fit in a hobby
// kernel: the expensive part of a real engine is the part that supports
// re-layout, and this one simply re-runs the pass when the width changes.

typedef struct {
    int32_t  x, y;          // page coordinates, origin at top-left
    int32_t  w, h;
    uint8_t  scale;         // 1 = body text, 2 = heading
    int32_t  link;          // index into links[], or -1
    uint32_t color;
    uint32_t off, len;      // slice of the text arena
} html_run_t;

typedef struct {
    int32_t x, y, w, h;
    uint32_t href_off;      // slice of the text arena holding the URL
    uint32_t href_len;
} html_link_t;

typedef struct {
    char*        text;
    uint32_t     text_len, text_cap;
    html_run_t*  runs;
    uint32_t     run_n, run_cap;
    html_link_t* links;
    uint32_t     link_n, link_cap;
    int32_t      height;
    char         title[128];

    // Target of a <meta http-equiv="refresh">, if the page had one. Plenty
    // of real pages redirect this way rather than with a 3xx status, and a
    // browser that ignores it shows a blank page instead of the content.
    char         refresh[URL_MAX];
} html_page_t;

// Lays out `len` bytes of markup into a page `width` pixels wide. Pass
// is_plain for text/plain, which skips tag handling entirely.
html_page_t* html_layout(const char* src, uint32_t len, int width, int is_plain);
void         html_free(html_page_t* p);

// Draws the part of the page visible in the given viewport rectangle.
void html_paint(const html_page_t* p, int vx, int vy, int vw, int vh, int scroll);

// Returns the index of the link at page coordinates (px, py), or -1.
int  html_hit_link(const html_page_t* p, int px, int py);

// Copies link `idx`'s href into out. Returns 0 on success.
int  html_link_href(const html_page_t* p, int idx, char* out, uint32_t cap);
