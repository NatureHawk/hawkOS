#pragma once
#include <stdint.h>
#include "header/http.h"
#include "header/fontprop.h"

// A laid-out page: a flat display list, not a DOM.
//
// Nothing here needs a tree. The layout pass walks the markup once, keeping a
// style stack, and emits positioned runs as it goes; painting is then a loop
// over the runs that intersect the viewport, and scrolling is a single
// offset. That is what makes a browser fit in a hobby kernel: the expensive
// part of a real engine is the part that supports re-layout, and this one
// simply re-runs the pass when the width changes.

// Faces the layout can select. This is an enum rather than a pf_face_t*
// because a run has to stay a plain value type -- the run array is grown by
// memcpy -- and because HTML_FACE_MONO names the fixed-cell console font,
// which is not a pf_face_t at all.
typedef enum {
    HTML_FACE_BODY = 0,
    HTML_FACE_BOLD,
    HTML_FACE_ITALIC,
    HTML_FACE_H3,
    HTML_FACE_H2,
    HTML_FACE_H1,
    HTML_FACE_MONO,
    HTML_FACE_N
} html_face_t;

// Per-run flags. Only what the painter needs to know that is not implied by
// the face or the colour.
#define HTML_RUN_NO_UNDERLINE  0x01   // a link whose CSS said text-decoration: none

typedef struct {
    int32_t  x, y;          // page coordinates; y is the top of the line box
    int32_t  w, h;
    uint8_t  face;          // html_face_t
    uint8_t  flags;         // HTML_RUN_*
    int32_t  link;          // index into links[], or -1
    uint32_t color;
    uint32_t off, len;      // slice of the text arena
} html_run_t;

// Everything on a page that is drawn rather than written: rules, bullets,
// the shaded ground behind a <pre>, a blockquote's edge, an image's frame.
//
// These live in their own array instead of being a variant of html_run_t for
// one reason that matters: text runs are emitted in increasing y, which is
// what lets html_paint stop scanning as soon as it passes the bottom of the
// viewport, and boxes are not -- a <pre> panel is only sized once its closing
// tag arrives, long after the text inside it was emitted. Keeping them apart
// preserves the ordering guarantee on the array where it pays for itself.
//
// It is also the array table borders and cell shading will be emitted into
// when tables land, which is why the kind is a byte with room to grow rather
// than a flag.
typedef enum {
    HTML_BOX_RULE = 0,      // <hr>, and the hairline under a major heading
    HTML_BOX_PANEL,         // the ground behind a <pre>
    HTML_BOX_BAR,           // a blockquote's left edge
    HTML_BOX_BULLET,        // an unordered-list marker
    HTML_BOX_FRAME,         // an image placeholder's border
    HTML_BOX_FIELD,         // a text input
    HTML_BOX_BUTTON         // a submit or push button
} html_box_kind_t;

typedef struct {
    int32_t  x, y, w, h;
    uint8_t  kind;          // html_box_kind_t
    uint32_t color;
} html_box_t;

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
    html_box_t*  boxes;
    uint32_t     box_n, box_cap;
    html_link_t* links;
    uint32_t     link_n, link_cap;
    int32_t      height;
    char         title[128];

    // Target of a <meta http-equiv="refresh">, if the page had one. Plenty
    // of real pages redirect this way rather than with a 3xx status, and a
    // browser that ignores it shows a blank page instead of the content.
    char         refresh[URL_MAX];
} html_page_t;

// The most a run's top can sit above the top of a run emitted before it.
// Baseline alignment moves a short face down within its line box, so runs
// are only non-decreasing in y to within the tallest line a page can set.
#define HTML_LINE_SLACK 64

// Lays out `len` bytes of markup into a page `width` pixels wide. Pass
// is_plain for text/plain, which skips tag handling entirely.
html_page_t* html_layout(const char* src, uint32_t len, int width, int is_plain);

// The same, with the reader-mode heuristics that drop navigation, footers
// and sidebars turned off. A page whose content is entirely inside elements
// that look like furniture renders as nothing at all under those rules, and
// showing its menus is better than showing a void -- see the fallback in
// app_browser.c.
html_page_t* html_layout_ex(const char* src, uint32_t len, int width, int is_plain,
                            int keep_chrome);

// Total characters across every run: how much text a page actually produced,
// which is how the browser tells "laid out fine" from "laid out to nothing".
uint32_t     html_text_len(const html_page_t* p);
void         html_free(html_page_t* p);

// Draws the part of the page visible in the given viewport rectangle.
void html_paint(const html_page_t* p, int vx, int vy, int vw, int vh, int scroll);

// Returns the index of the link at page coordinates (px, py), or -1.
int  html_hit_link(const html_page_t* p, int px, int py);

// Copies link `idx`'s href into out. Returns 0 on success.
int  html_link_href(const html_page_t* p, int idx, char* out, uint32_t cap);
