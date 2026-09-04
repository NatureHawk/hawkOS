// src/html.c — HTML tokeniser and single-pass layout
//
// This is a renderer, not a browser engine. It walks the markup once with a
// style stack and emits positioned runs; there is no DOM, no CSS cascade and
// no reflow. What that buys is that a page costs one linear pass and two flat
// arrays, which is what makes it viable to render Wikipedia inside a 128 MB
// budget on a hobby kernel.
//
// What it does handle, because pages are unreadable without it: block-level
// breaks, a real typographic scale for headings, bold and italic, lists,
// links, rules, blockquotes, preformatted text, image placeholders, entity
// decoding, and skipping the contents of <script> and <style> (which would
// otherwise dump source code into the middle of the text).
//
// Text is set in a proportional anti-aliased face (header/fontprop.h) and
// measured through the same code that draws it, so a line can never wrap at a
// width the painter disagrees with.
#include <stdint.h>
#include "header/html.h"
#include "header/gfx.h"
#include "header/wm.h"
#include "header/theme.h"
#include "header/kheap.h"
#include "header/kstring.h"
#include "header/font.h"
#include "header/fontprop.h"
#include "header/css.h"

#define TEXT_CAP_INIT  (64u * 1024u)
#define RUN_CAP_INIT   4096u
#define BOX_CAP_INIT   256u
#define LINK_CAP_INIT  512u

#define LINE_GAP       5
#define PARA_GAP       11
#define INDENT_STEP    26
#define MARGIN         14

// Cap on the measure, in pixels. Prose set across a full-width window runs to
// 150-odd characters a line, and the eye loses its place returning to the
// left edge; every typographic authority puts the comfortable range somewhere
// near 45-80 characters, and at this body size 700px lands inside it. Wider
// viewports get margins rather than longer lines.
#define MAX_LINE_W     700

// Pages are re-coloured for the dark theme rather than rendered on white. A
// browser that punched a bright rectangle into a dark desktop every time it
// loaded a page would be the one thing on screen you could not look at.
#define COL_BODY       GFX_RGB(0xD6, 0xDD, 0xE6)
#define COL_HEAD       GFX_RGB(0xFF, 0xFF, 0xFF)
#define COL_LINK       TH_LINK
#define COL_QUOTE      GFX_RGB(0x9A, 0xA6, 0xB4)
#define COL_RULE       GFX_RGB(0x2E, 0x39, 0x47)
#define COL_RULE_HEAD  GFX_RGB(0x3A, 0x47, 0x58)
#define COL_BAR        GFX_RGB(0x3E, 0x5A, 0x74)
#define COL_BULLET     GFX_RGB(0x7E, 0x8C, 0x9C)
#define COL_PANEL      GFX_RGB(0x11, 0x16, 0x1C)
#define COL_CODE       GFX_RGB(0xB9, 0xD8, 0xB0)
#define COL_FRAME      GFX_RGB(0x33, 0x40, 0x4E)
#define COL_FRAME_BG   GFX_RGB(0x13, 0x18, 0x20)
#define COL_ALT        GFX_RGB(0x74, 0x82, 0x92)

// ------------------------------------------------------------------- faces

static const pf_face_t* pf_of(int face){
    switch (face){
        case HTML_FACE_BOLD:   return &pf_body_bold;
        case HTML_FACE_ITALIC: return &pf_body_italic;
        case HTML_FACE_H3:     return &pf_h3;
        case HTML_FACE_H2:     return &pf_h2;
        case HTML_FACE_H1:     return &pf_h1;
        default:               return &pf_body;
    }
}

// The console font has no metrics table of its own; 13 is where the baseline
// sits in the VGA cell, and the extra leading keeps <pre> lines from touching.
#define MONO_H    (FONT_H + 3)
#define MONO_ASC  13

static int face_h(int face){
    return (face == HTML_FACE_MONO) ? MONO_H : (int)pf_of(face)->height;
}
static int face_asc(int face){
    return (face == HTML_FACE_MONO) ? MONO_ASC : (int)pf_of(face)->ascent;
}
static int measure(int face, const char* s, uint32_t n){
    if (face == HTML_FACE_MONO) return (int)n * FONT_W;
    return (int)gfx_pf_width_n(s, n, pf_of(face));
}
static int space_w(int face){
    return (face == HTML_FACE_MONO) ? FONT_W : (int)gfx_pf_width_n(" ", 1, pf_of(face));
}

// ------------------------------------------------------------- containers

static int arena_grow(html_page_t* p, uint32_t need){
    if (p->text_len + need <= p->text_cap) return 0;
    uint32_t cap = p->text_cap ? p->text_cap : TEXT_CAP_INIT;
    while (cap < p->text_len + need) cap *= 2;
    char* n = (char*)kmalloc(cap);
    if (!n) return -1;
    if (p->text) { memcpy(n, p->text, p->text_len); kfree(p->text); }
    p->text = n;
    p->text_cap = cap;
    return 0;
}

static uint32_t arena_put(html_page_t* p, const char* s, uint32_t len){
    if (arena_grow(p, len + 1) != 0) return 0xFFFFFFFFu;
    uint32_t off = p->text_len;
    memcpy(p->text + off, s, len);
    p->text[off + len] = 0;
    p->text_len += len + 1;
    return off;
}

static int runs_grow(html_page_t* p){
    if (p->run_n < p->run_cap) return 0;
    uint32_t cap = p->run_cap ? p->run_cap * 2 : RUN_CAP_INIT;
    html_run_t* n = (html_run_t*)kmalloc(cap * sizeof(html_run_t));
    if (!n) return -1;
    if (p->runs){ memcpy(n, p->runs, p->run_n * sizeof(html_run_t)); kfree(p->runs); }
    p->runs = n;
    p->run_cap = cap;
    return 0;
}

static int boxes_grow(html_page_t* p){
    if (p->box_n < p->box_cap) return 0;
    uint32_t cap = p->box_cap ? p->box_cap * 2 : BOX_CAP_INIT;
    html_box_t* n = (html_box_t*)kmalloc(cap * sizeof(html_box_t));
    if (!n) return -1;
    if (p->boxes){ memcpy(n, p->boxes, p->box_n * sizeof(html_box_t)); kfree(p->boxes); }
    p->boxes = n;
    p->box_cap = cap;
    return 0;
}

static int links_grow(html_page_t* p){
    if (p->link_n < p->link_cap) return 0;
    uint32_t cap = p->link_cap ? p->link_cap * 2 : LINK_CAP_INIT;
    html_link_t* n = (html_link_t*)kmalloc(cap * sizeof(html_link_t));
    if (!n) return -1;
    if (p->links){ memcpy(n, p->links, p->link_n * sizeof(html_link_t)); kfree(p->links); }
    p->links = n;
    p->link_cap = cap;
    return 0;
}

// ---------------------------------------------------------------- entities

typedef struct { const char* name; char ch; } entity_t;

static const entity_t ENTITIES[] = {
    { "amp",   '&'  }, { "lt",    '<'  }, { "gt",    '>'  },
    { "quot",  '"'  }, { "apos",  '\'' }, { "nbsp",  ' '  },
    { "mdash", '-'  }, { "ndash", '-'  }, { "hellip",'.'  },
    { "lsquo", '\'' }, { "rsquo", '\'' }, { "ldquo", '"'  }, { "rdquo", '"' },
    { "middot",'-'  }, { "times", 'x'  }, { "copy",  'c'  },
};
#define ENTITY_N ((int)(sizeof(ENTITIES) / sizeof(ENTITIES[0])))

// Decodes one entity starting at src[0] == '&'. Returns bytes consumed, and
// writes the replacement to *out (0 means "drop it").
static uint32_t decode_entity(const char* src, uint32_t avail, char* out){
    if (avail < 3 || src[0] != '&') return 0;

    uint32_t i = 1;
    while (i < avail && i < 12 && src[i] != ';' && src[i] != ' ' && src[i] != '<') i++;
    if (i >= avail || src[i] != ';') return 0;

    uint32_t nlen = i - 1;

    if (src[1] == '#'){
        // Numeric reference. Anything outside printable ASCII has no glyph in
        // the font tables, so it becomes '?' rather than a random one.
        uint32_t v = 0;
        uint32_t j = 2;
        if (j < i && (src[j] == 'x' || src[j] == 'X')){
            j++;
            for (; j < i; j++){
                char c = src[j];
                if      (c >= '0' && c <= '9') v = v * 16 + (uint32_t)(c - '0');
                else if (c >= 'a' && c <= 'f') v = v * 16 + (uint32_t)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') v = v * 16 + (uint32_t)(c - 'A' + 10);
                else return 0;
            }
        } else {
            for (; j < i; j++){
                char c = src[j];
                if (c < '0' || c > '9') return 0;
                v = v * 10 + (uint32_t)(c - '0');
            }
        }
        *out = (v >= 32 && v < 127) ? (char)v : ((v == 160) ? ' ' : '?');
        return i + 1;
    }

    for (int e = 0; e < ENTITY_N; e++){
        if (strlen(ENTITIES[e].name) == nlen && strncmp(src + 1, ENTITIES[e].name, nlen) == 0){
            *out = ENTITIES[e].ch;
            return i + 1;
        }
    }
    return 0;
}

// ------------------------------------------------------------------ layout

// The inherited properties of the element being laid out. This is deliberately
// shaped like the tail of a CSS cascade rather than like a set of flags: when
// style= and a <style> subset arrive, they set fields here and nothing else in
// the layout has to learn about them.
typedef struct {
    uint8_t  face;
    uint32_t color;
    int16_t  indent;
    uint8_t  pre;           // preserve whitespace, do not collapse it
    uint8_t  align;         // CSS_ALIGN_*; applied when a line is finished
    uint8_t  no_underline;  // text-decoration: none reached this element
} sty_t;

#define STY_MAX 32

// One entry per open element that changed the style. Frames also carry the
// decoration an element paints once its extent is known -- a blockquote's edge
// and a <pre>'s panel can only be sized at the closing tag.
typedef struct {
    char     tag[12];
    sty_t    saved;
    int32_t  y0;
    uint8_t  deco;          // html_box_kind_t, or DECO_NONE
    int16_t  deco_x;
} styframe_t;

#define DECO_NONE 0xFF

typedef struct {
    html_page_t* p;
    int   width;

    int   origin_x;         // left edge of the measure, in page coordinates
    int   content_w;        // width of the measure

    int   x, y;             // pen position
    int   line_h;           // tallest run on the current line
    int   line_asc;         // deepest baseline on the current line
    uint32_t line_first;    // first run index on the current line
    int   line_started;
    int   pending_space;    // collapsed whitespace waiting to be emitted

    sty_t sty;
    styframe_t stack[STY_MAX];
    int   depth;

    // The page's own stylesheet, gathered from its <style> elements and
    // consulted on every open tag. A flat rule array rather than anything
    // indexed, because indexing selectors needs a tree and there isn't one.
    css_sheet_t sheet;

    // Properties matched for the tag currently being opened, waiting for a
    // style frame to attach to. Carried on the layout rather than passed as an
    // argument because sty_push is called from a dozen places, and every one
    // of them wants the same thing done with it.
    css_props_t pending;
    int         has_pending;

    int   link;             // current link index, or -1

    int   ord[8];           // <ol> counters, one per nesting level
    int   list_depth;

    // Reader mode: while `skipping`, everything is discarded until the
    // matching close tag. Nesting is tracked by counting opens and closes of
    // the same tag name, which is enough for well-formed markup and cannot
    // run away on malformed markup because the counter only ever decreases
    // on a close.
    int   skipping;
    char  skip_tag[16];
    int   skip_nest;
} lay_t;

static int line_left(lay_t* L){ return L->origin_x + L->sty.indent; }
static int line_right(lay_t* L){ return L->origin_x + L->content_w; }

static void emit_box(lay_t* L, int kind, int x, int y, int w, int h, uint32_t color){
    if (w <= 0 || h <= 0) return;
    if (boxes_grow(L->p) != 0) return;
    html_box_t* b = &L->p->boxes[L->p->box_n++];
    b->x = x; b->y = y; b->w = w; b->h = h;
    b->kind = (uint8_t)kind;
    b->color = color;
}

// Baseline-aligns everything on the line just finished. A run's y is the top
// of its line box, so a 29px heading and 17px body text sharing a line would
// otherwise be aligned by their tops and visibly sit on different baselines.
// Doing it here rather than at emit time is what makes it correct: the
// deepest baseline on a line is not known until the line ends.
static void align_line(lay_t* L){
    // Horizontal alignment, if the element asked for anything but the default.
    // A line's width is only known once the line is finished, which is why
    // this happens here rather than at emit time -- the same reason the
    // baseline pass is here.
    int shift = 0;
    if (L->sty.align != CSS_ALIGN_LEFT && L->p->run_n > L->line_first){
        int32_t lo = 0x7FFFFFFF, hi = 0;
        for (uint32_t i = L->line_first; i < L->p->run_n; i++){
            const html_run_t* r = &L->p->runs[i];
            if (r->x < lo) lo = r->x;
            if (r->x + r->w > hi) hi = r->x + r->w;
        }
        int avail = line_right(L) - line_left(L);
        int used  = hi - lo;
        if (used > 0 && used < avail)
            shift = (L->sty.align == CSS_ALIGN_CENTER) ? (avail - used) / 2
                                                       : (avail - used);
    }

    for (uint32_t i = L->line_first; i < L->p->run_n; i++){
        html_run_t* r = &L->p->runs[i];
        r->y += L->line_asc - face_asc(r->face);
        r->x += shift;
    }
}

static void newline(lay_t* L, int extra){
    if (L->line_started || extra){
        if (L->line_started) align_line(L);
        L->y += (L->line_h ? L->line_h : face_h(HTML_FACE_BODY)) + LINE_GAP + extra;
    }
    L->x = line_left(L);
    L->line_h = 0;
    L->line_asc = 0;
    L->line_first = L->p->run_n;
    L->line_started = 0;
    L->pending_space = 0;
}

// Ends the current line only if something is on it, so consecutive block
// tags (</p><div>) do not stack up blank lines.
static void block_break(lay_t* L, int extra){
    if (L->line_started) newline(L, extra);
    else {
        if (extra) L->y += extra;
        L->x = line_left(L);
        L->line_first = L->p->run_n;
    }
}

static void emit_word(lay_t* L, const char* word, uint32_t len){
    if (!len) return;

    int face = L->sty.face;
    int wpx  = measure(face, word, len);
    int ch   = face_h(face);
    int sp   = (L->pending_space && L->line_started) ? space_w(face) : 0;

    if (L->line_started && L->x + sp + wpx > line_right(L)){
        newline(L, 0);
        sp = 0;
    }
    // A single word longer than the measure still has to go somewhere; let it
    // overflow its line rather than looping forever trying to fit it.
    L->x += sp;

    if (runs_grow(L->p) != 0) return;
    uint32_t off = arena_put(L->p, word, len);
    if (off == 0xFFFFFFFFu) return;

    html_run_t* r = &L->p->runs[L->p->run_n++];
    r->x = L->x;  r->y = L->y;
    r->w = wpx;   r->h = ch;
    r->face  = (uint8_t)face;
    r->flags = L->sty.no_underline ? HTML_RUN_NO_UNDERLINE : 0;
    r->link  = L->link;
    r->color = L->sty.color;
    r->off = off;
    r->len = len;

    L->x += wpx;
    if (ch > L->line_h) L->line_h = ch;
    if (face_asc(face) > L->line_asc) L->line_asc = face_asc(face);
    L->line_started = 1;
    L->pending_space = 0;
}

// ------------------------------------------------------------- style stack

static int tag_is(const char* tag, uint32_t len, const char* name);

static void apply_css(lay_t* L, const css_props_t* c);

static void sty_push(lay_t* L, const char* tag, uint32_t nlen, int deco, int deco_x){
    if (L->depth >= STY_MAX) return;
    styframe_t* f = &L->stack[L->depth++];
    uint32_t keep = nlen < sizeof(f->tag) - 1 ? nlen : sizeof(f->tag) - 1;
    memcpy(f->tag, tag, keep);
    f->tag[keep] = 0;
    f->saved  = L->sty;
    f->y0     = L->y;
    f->deco   = (uint8_t)deco;
    f->deco_x = (int16_t)deco_x;

    // Any CSS matched for this element lands here, on the frame that will
    // restore the previous style when it closes. An element's own semantics
    // are applied by its handler afterwards, so a <b> stays bold even if a
    // stylesheet disagrees -- a deliberate simplification, and the safe
    // direction to be wrong in.
    if (L->has_pending){
        apply_css(L, &L->pending);
        L->has_pending = 0;
    }
}

static void frame_close(lay_t* L, styframe_t* f){
    switch (f->deco){
        case HTML_BOX_BAR:
            emit_box(L, HTML_BOX_BAR, f->deco_x, f->y0, 3, L->y - f->y0, COL_BAR);
            break;
        case HTML_BOX_PANEL:
            emit_box(L, HTML_BOX_PANEL, f->deco_x - 10, f->y0 - 7,
                     line_right(L) - (f->deco_x - 10) + 4, L->y - f->y0 + 14, COL_PANEL);
            break;
        default: break;
    }
    L->sty = f->saved;
}

// True if `tag` has a frame open. The generic close-tag path needs this to
// tell "this element styled itself and must be popped" from "this is one of
// the thousands of </div>s that changed nothing".
static int sty_has(lay_t* L, const char* tag, uint32_t nlen){
    for (int i = L->depth - 1; i >= 0; i--)
        if (tag_is(tag, nlen, L->stack[i].tag)) return 1;
    return 0;
}

// Folds a set of CSS properties into the current style. Faces are chosen
// here, because the layout has five of them and CSS has a continuum: a size
// is snapped to the nearest face rather than scaling a bitmap, which would
// look worse than picking the wrong one.
static void apply_css(lay_t* L, const css_props_t* c){
    if (c->have & CSS_HAS_COLOR) L->sty.color = c->color;

    if (c->have & CSS_HAS_INDENT){
        int ind = L->sty.indent + c->indent;
        if (ind < 0) ind = 0;
        L->sty.indent = (int16_t)ind;
        L->x = line_left(L);
    }
    if (c->have & CSS_HAS_ALIGN)     L->sty.align = c->align;
    if (c->have & CSS_HAS_UNDERLINE) L->sty.no_underline = (uint8_t)(!c->underline);

    // Size first, then weight and slant, so "bold, 28px" lands on a heading
    // face rather than on body bold.
    int face = L->sty.face;
    if (c->have & CSS_HAS_SIZE){
        int px = c->size_px;
        if      (px >= 25) face = HTML_FACE_H1;
        else if (px >= 20) face = HTML_FACE_H2;
        else if (px >= 18) face = HTML_FACE_H3;
        else               face = HTML_FACE_BODY;
    }
    if (c->have & CSS_HAS_MONO && c->mono) face = HTML_FACE_MONO;
    if ((c->have & CSS_HAS_WEIGHT) && c->bold
        && (face == HTML_FACE_BODY || face == HTML_FACE_ITALIC)) face = HTML_FACE_BOLD;
    if ((c->have & CSS_HAS_ITALIC) && c->italic
        && (face == HTML_FACE_BODY || face == HTML_FACE_BOLD)) face = HTML_FACE_ITALIC;
    if ((c->have & CSS_HAS_WEIGHT) && !c->bold && face == HTML_FACE_BOLD)
        face = HTML_FACE_BODY;
    L->sty.face = (uint8_t)face;
}

// Closes the topmost frame for `tag`, and anything left open inside it. Real
// pages leave tags unclosed constantly; unwinding to the match rather than
// popping blindly is what stops one stray </div> from resetting the style of
// the rest of the document.
static void sty_pop(lay_t* L, const char* tag, uint32_t nlen){
    int at = -1;
    for (int i = L->depth - 1; i >= 0; i--){
        if (tag_is(tag, nlen, L->stack[i].tag)){ at = i; break; }
    }
    if (at < 0) return;
    for (int i = L->depth - 1; i >= at; i--) frame_close(L, &L->stack[i]);
    L->depth = at;
}

// ------------------------------------------------------------------- tags

static int tag_is(const char* tag, uint32_t len, const char* name){
    return strlen(name) == len && kstrnicmp(tag, name, len) == 0;
}

// Reads attribute `want` out of a tag's attribute text.
static int tag_attr(const char* attrs, uint32_t len, const char* want,
                    char* out, uint32_t cap){
    uint32_t wlen = strlen(want);
    uint32_t i = 0;
    out[0] = 0;

    while (i < len){
        while (i < len && (attrs[i] == ' ' || attrs[i] == '\t' || attrs[i] == '\n' || attrs[i] == '\r')) i++;
        uint32_t ns = i;
        while (i < len && attrs[i] != '=' && attrs[i] != ' ' && attrs[i] != '>') i++;
        uint32_t nlen = i - ns;

        if (i < len && attrs[i] == '='){
            i++;
            char quote = 0;
            if (i < len && (attrs[i] == '"' || attrs[i] == '\'')){ quote = attrs[i]; i++; }
            uint32_t vs = i;
            while (i < len && ((quote && attrs[i] != quote) || (!quote && attrs[i] != ' '))) i++;
            uint32_t vlen = i - vs;
            if (quote && i < len) i++;

            if (nlen == wlen && kstrnicmp(attrs + ns, want, wlen) == 0){
                uint32_t n = vlen < cap - 1 ? vlen : cap - 1;
                // Decode entities in attribute values too: &amp; is extremely
                // common inside query strings.
                uint32_t o = 0;
                for (uint32_t k = 0; k < n && o < cap - 1; k++){
                    char rep;
                    uint32_t used = decode_entity(attrs + vs + k, n - k, &rep);
                    if (used){ out[o++] = rep; k += used - 1; }
                    else out[o++] = attrs[vs + k];
                }
                out[o] = 0;
                return 0;
            }
        }
        if (i < len && attrs[i] != '=') i++;
    }
    return -1;
}

// Page furniture: navigation, sidebars, footers and controls. Dropping it is
// a heuristic, not a rule -- there is no way to be certain what is content --
// but without it a modern page renders as several screens of menu links
// before the first sentence of what you actually opened.
static int is_chrome_tag(const char* t, uint32_t n){
    // Deliberately not "form": some sites (classic ASP.NET in particular)
    // wrap the entire page body in one, and skipping it would render them
    // blank rather than merely cluttered.
    static const char* const C[] = {
        "nav", "footer", "aside", "noscript", "svg", "template",
        "select", "button", "iframe"
    };
    for (int i = 0; i < (int)(sizeof(C)/sizeof(C[0])); i++)
        if (tag_is(t, n, C[i])) return 1;
    return 0;
}

// The same judgement applied to class and id names. The entries are chosen
// to be specific enough not to swallow content: "mw-portlet" is Wikipedia's
// sidebar menus, while its article body is "mw-body-content" and survives.
//
// This list was meant to shrink once CSS landed, and for pages that carry
// their rules in a <style> element it has: display:none in the page's own
// stylesheet now does the same job on evidence rather than on guesswork. It
// has not gone away because the sites with the most furniture keep their
// rules in an external stylesheet, and fetching one costs a second round trip
// and, on Wikipedia, most of a megabyte -- to hide some [edit] links. Until
// that trade is worth making, the names below stay.
static int is_chrome_attr(const char* attrs, uint32_t alen){
    static const char* const C[] = {
        "mw-portlet", "vector-dropdown", "vector-menu", "vector-toc",
        "vector-page-tools", "vector-header", "vector-sticky",
        "mw-editsection", "mw-cite-backlink", "reference-text",
        "mw-jump-link", "printfooter", "catlinks", "navbox", "noprint",
        "sidebar", "breadcrumb", "cookie", "skip-link", "site-footer",
        "page-footer", "site-nav", "global-nav"
    };
    char v[192];
    for (int a = 0; a < 2; a++){
        if (tag_attr(attrs, alen, a ? "id" : "class", v, sizeof(v)) != 0) continue;
        for (int i = 0; i < (int)(sizeof(C)/sizeof(C[0])); i++)
            if (strstr(v, C[i])) return 1;
    }
    return 0;
}

static int is_block_tag(const char* t, uint32_t n){
    static const char* B[] = {
        "p","div","br","hr","h1","h2","h3","h4","h5","h6","ul","ol","li",
        "table","tr","thead","tbody","section","article","header","footer",
        "nav","main","aside","blockquote","pre","form","figure","figcaption",
        "dl","dt","dd","address","fieldset","details","summary"
    };
    for (int i = 0; i < (int)(sizeof(B)/sizeof(B[0])); i++)
        if (tag_is(t, n, B[i])) return 1;
    return 0;
}

// Heading levels 1..6, or 0 for anything else.
static int heading_level(const char* t, uint32_t n){
    if (n != 2 || (t[0] != 'h' && t[0] != 'H')) return 0;
    if (t[1] < '1' || t[1] > '6') return 0;
    return t[1] - '0';
}

// ------------------------------------------------------------------ images

static int attr_int(const char* attrs, uint32_t alen, const char* name){
    char v[16];
    if (tag_attr(attrs, alen, name, v, sizeof(v)) != 0) return -1;
    int n = 0, any = 0;
    for (int i = 0; v[i]; i++){
        if (v[i] < '0' || v[i] > '9') break;
        n = n * 10 + (v[i] - '0');
        any = 1;
        if (n > 4096) return 4096;
    }
    return any ? n : -1;
}

// Emits an already-decoded string as wrapped words, in whatever style is
// current. Used for text that reaches the layout through an attribute rather
// than through the character loop.
static void emit_words(lay_t* L, const char* s){
    uint32_t i = 0;
    while (s[i]){
        while (s[i] == ' '){ L->pending_space = 1; i++; }
        uint32_t j = i;
        while (s[j] && s[j] != ' ') j++;
        if (j > i) emit_word(L, s + i, j - i);
        i = j;
    }
}

// There is no image decoder yet, so an <img> becomes a frame the size the
// markup asked for with its alt text inside it. That is not a placeholder for
// its own sake: it keeps the page's vertical rhythm honest, and alt text is
// frequently the caption a reader actually wanted. When a decoder lands it
// draws into exactly this rectangle.
static void emit_image(lay_t* L, const char* attrs, uint32_t alen){
    int aw = attr_int(attrs, alen, "width");
    int ah = attr_int(attrs, alen, "height");

    // Tracking pixels and spacer gifs are images in name only.
    if ((aw >= 0 && aw <= 2) || (ah >= 0 && ah <= 2)) return;

    char alt[160];
    if (tag_attr(attrs, alen, "alt", alt, sizeof(alt)) != 0 || !alt[0]){
        alt[0] = 0;
    }

    // An icon-sized image is furniture -- a padlock, a flag, a rating star --
    // and framing it drops an empty box into the middle of a sentence.
    // Wikipedia's article-protection padlock is exactly this case. It either
    // contributes its alt text inline or it contributes nothing.
    if ((aw > 0 && aw < 48) || (ah > 0 && ah < 48)){
        if (!alt[0]) return;
        sty_t save = L->sty;
        L->sty.face  = HTML_FACE_ITALIC;
        L->sty.color = COL_ALT;
        emit_words(L, alt);
        L->sty = save;
        L->pending_space = 1;
        return;
    }

    // A great many real images carry no alt at all. Falling back to the file
    // name turns a dead rectangle into something that says what is missing --
    // "IBM System360 Model 50" is most of what the alt would have said anyway.
    // This deliberately sits below the icon test: a decorative icon with no
    // alt should disappear, not announce its file name mid-sentence.
    if (!alt[0]){
        char src[URL_MAX];
        if (tag_attr(attrs, alen, "src", src, sizeof(src)) == 0 && src[0]){
            uint32_t q = 0;
            while (src[q] && src[q] != '?' && src[q] != '#') q++;
            uint32_t b = q;
            while (b > 0 && src[b - 1] != '/') b--;
            // Thumbnail URLs carry a "250px-" size prefix that is about the
            // scaler, not the picture. Drop it.
            uint32_t d = b;
            while (d < q && src[d] >= '0' && src[d] <= '9') d++;
            if (d > b && d + 3 <= q && src[d] == 'p' && src[d+1] == 'x' && src[d+2] == '-')
                b = d + 3;
            uint32_t e = q;
            while (e > b && src[e - 1] != '.') e--;
            if (e > b + 1) q = e - 1;                 // drop the extension
            uint32_t o = 0;
            for (uint32_t k = b; k < q && o < sizeof(alt) - 1; k++)
                alt[o++] = (src[k] == '_' || src[k] == '-') ? ' ' : src[k];
            alt[o] = 0;
        }
        if (!alt[0]) strncpy(alt, "image", sizeof(alt) - 1);
    }

    int avail = line_right(L) - line_left(L);
    int w = (aw > 0 ? aw : 220);
    int h = (ah > 0 ? ah : 130);
    if (w > avail) {
        // Preserve the aspect ratio the markup declared when scaling down, so
        // a wide banner does not become a square.
        if (aw > 0 && ah > 0) h = (int)(((int64_t)h * avail) / w);
        w = avail;
    }
    if (h < 30) h = 30;
    if (h > 320) h = 320;

    block_break(L, PARA_GAP);
    int x = line_left(L);
    emit_box(L, HTML_BOX_FRAME, x, L->y, w, h, COL_FRAME);

    if (alt[0]){
        // One truncated line rather than a wrapped block: the frame is
        // furniture, and text spilling out of it would look like a bug.
        uint32_t n = strlen(alt);
        while (n > 1 && measure(HTML_FACE_ITALIC, alt, n) > w - 20) n--;
        if (runs_grow(L->p) == 0){
            uint32_t off = arena_put(L->p, alt, n);
            if (off != 0xFFFFFFFFu){
                html_run_t* r = &L->p->runs[L->p->run_n++];
                r->x = x + 10;
                r->y = L->y + (h - face_h(HTML_FACE_ITALIC)) / 2;
                r->w = measure(HTML_FACE_ITALIC, alt, n);
                r->h = face_h(HTML_FACE_ITALIC);
                r->face  = HTML_FACE_ITALIC;
                r->flags = 0;
                r->link  = L->link;
                r->color = COL_ALT;
                r->off = off;
                r->len = n;
            }
        }
    }

    L->y += h + PARA_GAP;
    L->x = line_left(L);
    L->line_first = L->p->run_n;
    L->line_h = 0;
    L->line_asc = 0;
    L->line_started = 0;
    L->pending_space = 0;
}

// ---------------------------------------------------------------- entry

html_page_t* html_layout(const char* src, uint32_t len, int width, int is_plain){
    html_page_t* p = (html_page_t*)kmalloc(sizeof(html_page_t));
    if (!p) return 0;
    memset(p, 0, sizeof(*p));

    lay_t L;
    memset(&L, 0, sizeof(L));
    L.p = p;
    L.width = width;

    int avail = width - 2 * MARGIN;
    if (avail < 120) avail = 120;
    L.content_w = avail > MAX_LINE_W ? MAX_LINE_W : avail;
    L.origin_x  = MARGIN + (avail - L.content_w) / 2;

    L.sty.face   = is_plain ? HTML_FACE_MONO : HTML_FACE_BODY;
    L.sty.color  = COL_BODY;
    L.sty.indent = 0;
    L.sty.pre    = is_plain ? 1 : 0;
    L.x = L.origin_x;
    L.y = MARGIN;
    L.link = -1;
    // -1 means "unordered". A stray <li> outside any list gets a bullet
    // rather than being numbered from a counter nothing ever opened.
    for (int d = 0; d < 8; d++) L.ord[d] = -1;

    char word[256];
    uint32_t wlen = 0;
    int in_title = 0;
    uint32_t title_len = 0;

    uint32_t i = 0;
    while (i < len){
        char c = src[i];

        if (!is_plain && c == '<'){
            // Flush whatever word was being accumulated before the tag.
            if (wlen){ emit_word(&L, word, wlen); wlen = 0; }

            if (i + 3 < len && src[i+1] == '!' && src[i+2] == '-' && src[i+3] == '-'){
                uint32_t j = i + 4;
                while (j + 2 < len && !(src[j] == '-' && src[j+1] == '-' && src[j+2] == '>')) j++;
                i = (j + 3 < len) ? j + 3 : len;
                continue;
            }

            uint32_t ts = i + 1;
            int closing = 0;
            if (ts < len && src[ts] == '/'){ closing = 1; ts++; }

            uint32_t ne = ts;
            while (ne < len && src[ne] != '>' && src[ne] != ' ' && src[ne] != '\t'
                            && src[ne] != '\n' && src[ne] != '\r' && src[ne] != '/') ne++;
            uint32_t nlen = ne - ts;

            uint32_t te = ne;
            while (te < len && src[te] != '>') te++;

            const char* tag   = src + ts;
            const char* attrs = src + ne;
            uint32_t    alen  = te > ne ? te - ne : 0;

            if (nlen == 0){ i = (te < len) ? te + 1 : len; continue; }

            if (L.skipping){
                if (tag_is(tag, nlen, L.skip_tag)){
                    if (closing){ if (--L.skip_nest <= 0) L.skipping = 0; }
                    else L.skip_nest++;
                }
                // Safety valve for markup where the close tag never arrives:
                // without this, one unterminated <nav> would swallow the rest
                // of the document.
                else if (closing && (tag_is(tag, nlen, "body") || tag_is(tag, nlen, "html")))
                    L.skipping = 0;

                i = (te < len) ? te + 1 : len;
                continue;
            }

            // What the page's own stylesheet and style= attribute say about
            // this element. Computed once here and used by both the skip
            // decision below and the style push further down.
            css_props_t cp;
            memset(&cp, 0, sizeof(cp));
            int have_css = 0;
            if (!closing && (L.sheet.n || alen)){
                char cls[192], idv[64];
                if (tag_attr(attrs, alen, "class", cls, sizeof(cls)) != 0) cls[0] = 0;
                if (tag_attr(attrs, alen, "id", idv, sizeof(idv)) != 0) idv[0] = 0;
                if (L.sheet.n) css_match(&L.sheet, tag, nlen, cls, idv, &cp);

                char inl[256];
                if (tag_attr(attrs, alen, "style", inl, sizeof(inl)) == 0 && inl[0]){
                    css_props_t ip;
                    memset(&ip, 0, sizeof(ip));
                    css_parse_inline(inl, strlen(inl), &ip);
                    css_merge(&cp, &ip);      // inline beats the sheet
                }
                have_css = cp.have != 0;
            }
            L.has_pending = 0;
            if (have_css){ L.pending = cp; L.has_pending = 1; }

            // display:none is the page telling us, in its own words, that this
            // subtree is not content. It is a far better signal than the
            // hardcoded list of class names below, which only ever knew about
            // the sites someone had already looked at.
            if (!closing && have_css && cp.hidden && src[te - 1] != '/'
                && !tag_is(tag, nlen, "body") && !tag_is(tag, nlen, "html")){
                block_break(&L, 0);
                L.skipping  = 1;
                L.skip_nest = 1;
                uint32_t keep = nlen < sizeof(L.skip_tag) - 1 ? nlen : sizeof(L.skip_tag) - 1;
                memcpy(L.skip_tag, tag, keep);
                L.skip_tag[keep] = 0;
                i = (te < len) ? te + 1 : len;
                continue;
            }

            if (!closing && src[te - 1] != '/'
                && (is_chrome_tag(tag, nlen) || is_chrome_attr(attrs, alen))){
                block_break(&L, 0);
                L.skipping  = 1;
                L.skip_nest = 1;
                uint32_t keep = nlen < sizeof(L.skip_tag) - 1 ? nlen : sizeof(L.skip_tag) - 1;
                memcpy(L.skip_tag, tag, keep);
                L.skip_tag[keep] = 0;
                i = (te < len) ? te + 1 : len;
                continue;
            }

            // <script> holds code, not prose: skip it. <style> holds rules,
            // which used to be skipped the same way -- and dropping them meant
            // dropping the page's own statement about what is content and what
            // is furniture. Its text goes to the stylesheet instead.
            if (!closing && (tag_is(tag, nlen, "script") || tag_is(tag, nlen, "style"))){
                int is_style = tag_is(tag, nlen, "style");
                const char* close = is_style ? "</style" : "</script";
                uint32_t clen = strlen(close);
                uint32_t body = (te < len) ? te + 1 : len;
                uint32_t j = body;
                while (j + clen < len && kstrnicmp(src + j, close, clen) != 0) j++;
                if (is_style && j > body) css_sheet_add(&L.sheet, src + body, j - body);
                while (j < len && src[j] != '>') j++;
                i = (j < len) ? j + 1 : len;
                continue;
            }

            // An external stylesheet cannot be fetched from inside the layout
            // pass, so <link rel=stylesheet> is simply not honoured; a page
            // that keeps all its rules in one is rendered from its markup
            // alone, which is what happened before any of this existed.

            if (tag_is(tag, nlen, "meta") && !closing){
                char eq[24], content[URL_MAX];
                if (tag_attr(attrs, alen, "http-equiv", eq, sizeof(eq)) == 0
                    && kstricmp(eq, "refresh") == 0
                    && tag_attr(attrs, alen, "content", content, sizeof(content)) == 0){
                    // content looks like "0; url=https://..."
                    const char* u = content;
                    while (*u && kstrnicmp(u, "url", 3) != 0) u++;
                    if (*u){
                        u += 3;
                        while (*u == ' ' || *u == '=' || *u == '\'' || *u == '"') u++;
                        uint32_t n = 0;
                        while (u[n] && u[n] != '\'' && u[n] != '"' && n < URL_MAX - 1) n++;
                        memcpy(p->refresh, u, n);
                        p->refresh[n] = 0;
                    }
                }
                i = (te < len) ? te + 1 : len;
                continue;
            }

            if (tag_is(tag, nlen, "title")){
                in_title = !closing;
                i = (te < len) ? te + 1 : len;
                continue;
            }

            if (tag_is(tag, nlen, "img") && !closing){
                emit_image(&L, attrs, alen);
                i = (te < len) ? te + 1 : len;
                continue;
            }

            if (tag_is(tag, nlen, "a")){
                if (closing){
                    L.link = -1;
                    sty_pop(&L, tag, nlen);
                } else {
                    char href[URL_MAX];
                    if (tag_attr(attrs, alen, "href", href, sizeof(href)) == 0 && href[0]
                        && href[0] != '#' && kstrnicmp(href, "javascript:", 11) != 0){
                        if (links_grow(p) == 0){
                            uint32_t off = arena_put(p, href, strlen(href));
                            if (off != 0xFFFFFFFFu){
                                html_link_t* lk = &p->links[p->link_n];
                                memset(lk, 0, sizeof(*lk));
                                lk->href_off = off;
                                lk->href_len = strlen(href);
                                L.link = (int)p->link_n++;
                                sty_push(&L, tag, nlen, DECO_NONE, 0);
                                L.sty.color = COL_LINK;
                            }
                        }
                    }
                }
                i = (te < len) ? te + 1 : len;
                continue;
            }

            // ------------------------------------------------- inline style

            if (tag_is(tag, nlen, "b") || tag_is(tag, nlen, "strong")
             || tag_is(tag, nlen, "th")){
                if (closing) sty_pop(&L, tag, nlen);
                else { sty_push(&L, tag, nlen, DECO_NONE, 0); L.sty.face = HTML_FACE_BOLD; }
                if (tag_is(tag, nlen, "th")) block_break(&L, 0);
                i = (te < len) ? te + 1 : len;
                continue;
            }

            if (tag_is(tag, nlen, "i") || tag_is(tag, nlen, "em")
             || tag_is(tag, nlen, "cite") || tag_is(tag, nlen, "dfn")){
                if (closing) sty_pop(&L, tag, nlen);
                else { sty_push(&L, tag, nlen, DECO_NONE, 0); L.sty.face = HTML_FACE_ITALIC; }
                i = (te < len) ? te + 1 : len;
                continue;
            }

            if (tag_is(tag, nlen, "code") || tag_is(tag, nlen, "kbd")
             || tag_is(tag, nlen, "samp") || tag_is(tag, nlen, "tt")){
                if (closing) sty_pop(&L, tag, nlen);
                else {
                    sty_push(&L, tag, nlen, DECO_NONE, 0);
                    L.sty.face  = HTML_FACE_MONO;
                    if (L.link < 0) L.sty.color = COL_CODE;
                }
                i = (te < len) ? te + 1 : len;
                continue;
            }

            // ------------------------------------------------------- blocks

            if (tag_is(tag, nlen, "pre")){
                if (closing){
                    block_break(&L, 0);
                    sty_pop(&L, tag, nlen);
                    L.y += PARA_GAP;
                } else {
                    block_break(&L, PARA_GAP + 4);
                    sty_push(&L, tag, nlen, HTML_BOX_PANEL, line_left(&L));
                    L.sty.face = HTML_FACE_MONO;
                    L.sty.pre  = 1;
                    if (L.link < 0) L.sty.color = COL_CODE;
                }
                i = (te < len) ? te + 1 : len;
                continue;
            }

            int hl = heading_level(tag, nlen);
            if (hl){
                if (closing){
                    block_break(&L, 0);
                    sty_pop(&L, tag, nlen);
                    // A hairline under the top two levels does the work a
                    // size jump alone cannot: it separates a section from the
                    // one above it without another blank line's worth of gap.
                    if (hl <= 2){
                        L.y += 6;
                        emit_box(&L, HTML_BOX_RULE, line_left(&L), L.y,
                                 line_right(&L) - line_left(&L), 1, COL_RULE_HEAD);
                        L.y += 9;
                    } else {
                        L.y += 4;
                    }
                } else {
                    block_break(&L, hl <= 2 ? PARA_GAP + 9 : PARA_GAP + 4);
                    sty_push(&L, tag, nlen, DECO_NONE, 0);
                    L.sty.face  = (hl == 1) ? HTML_FACE_H1
                                : (hl == 2) ? HTML_FACE_H2
                                : (hl == 3) ? HTML_FACE_H3
                                            : HTML_FACE_BOLD;
                    L.sty.color = COL_HEAD;
                }
                i = (te < len) ? te + 1 : len;
                continue;
            }

            if (tag_is(tag, nlen, "li")){
                if (!closing){
                    block_break(&L, 2);
                    int d = L.list_depth > 0 ? L.list_depth - 1 : 0;
                    if (L.ord[d] >= 0){
                        char num[8];
                        int n = ++L.ord[d];
                        int k = 0;
                        if (n >= 100) num[k++] = (char)('0' + (n / 100) % 10);
                        if (n >= 10)  num[k++] = (char)('0' + (n / 10) % 10);
                        num[k++] = (char)('0' + n % 10);
                        num[k++] = '.';
                        // The marker hangs in the indent rather than pushing
                        // the text right, so wrapped lines align under the
                        // first word instead of under the number.
                        L.x = line_left(&L) - measure(L.sty.face, num, (uint32_t)k) - 8;
                        emit_word(&L, num, (uint32_t)k);
                        L.x = line_left(&L);
                        L.pending_space = 0;
                    } else {
                        emit_box(&L, HTML_BOX_BULLET, line_left(&L) - 14,
                                 L.y + face_asc(L.sty.face) - 8, 5, 5, COL_BULLET);
                    }
                }
                i = (te < len) ? te + 1 : len;
                continue;
            }

            if (tag_is(tag, nlen, "ul") || tag_is(tag, nlen, "ol")){
                if (closing){
                    sty_pop(&L, tag, nlen);
                    if (L.list_depth > 0) L.list_depth--;
                    block_break(&L, PARA_GAP);
                } else {
                    block_break(&L, 4);
                    sty_push(&L, tag, nlen, DECO_NONE, 0);
                    L.sty.indent = (int16_t)(L.sty.indent + INDENT_STEP);
                    if (L.list_depth < 8){
                        L.ord[L.list_depth] = tag_is(tag, nlen, "ol") ? 0 : -1;
                        L.list_depth++;
                    }
                }
                L.x = line_left(&L);
                i = (te < len) ? te + 1 : len;
                continue;
            }

            if (tag_is(tag, nlen, "blockquote")){
                if (closing){
                    block_break(&L, 0);
                    sty_pop(&L, tag, nlen);
                    L.y += PARA_GAP;
                } else {
                    block_break(&L, PARA_GAP);
                    sty_push(&L, tag, nlen, HTML_BOX_BAR, line_left(&L));
                    L.sty.indent = (int16_t)(L.sty.indent + INDENT_STEP);
                    L.sty.color  = COL_QUOTE;
                }
                L.x = line_left(&L);
                i = (te < len) ? te + 1 : len;
                continue;
            }

            if (tag_is(tag, nlen, "br")){ newline(&L, 0); i = (te < len) ? te + 1 : len; continue; }

            if (tag_is(tag, nlen, "hr")){
                block_break(&L, PARA_GAP);
                emit_box(&L, HTML_BOX_RULE, line_left(&L), L.y,
                         line_right(&L) - line_left(&L), 1, COL_RULE);
                L.y += PARA_GAP;
                i = (te < len) ? te + 1 : len;
                continue;
            }

            // Anything with styling of its own that no specific handler above
            // claimed. This is where a <div>, <span> or <td> carrying a colour,
            // a weight or an alignment gets it applied.
            if (have_css && src[te - 1] != '/'){
                if (is_block_tag(tag, nlen))
                    block_break(&L, tag_is(tag, nlen, "p") ? PARA_GAP : 0);
                sty_push(&L, tag, nlen, DECO_NONE, 0);   // consumes the pending props
                i = (te < len) ? te + 1 : len;
                continue;
            }
            if (closing && sty_has(&L, tag, nlen)){
                if (is_block_tag(tag, nlen)) block_break(&L, 0);
                sty_pop(&L, tag, nlen);
                i = (te < len) ? te + 1 : len;
                continue;
            }

            if (is_block_tag(tag, nlen)) block_break(&L, tag_is(tag, nlen, "p") ? PARA_GAP : 0);

            i = (te < len) ? te + 1 : len;
            continue;
        }

        if (L.skipping){ i++; continue; }

        // <title> is metadata, not content, so it is collected rather than laid
        // out. This has to come before the whitespace rule below: when it did
        // not, every space in a title was swallowed by the collapsing branch
        // and the browser's tabs read "Operatingsystem" instead of
        // "Operating system".
        if (in_title){
            char tc = c;
            if (tc == '\t' || tc == '\r' || tc == '\n') tc = ' ';
            if (!is_plain && tc == '&'){
                char rep;
                uint32_t used = decode_entity(src + i, len - i, &rep);
                if (used){ tc = rep; i += used - 1; }
            }
            if (tc >= 32 && tc < 127){
                int trailing_space = (title_len > 0 && p->title[title_len - 1] == ' ');
                // Collapse runs of whitespace, and never open with one.
                if (tc != ' ' || (title_len > 0 && !trailing_space)){
                    if (title_len < sizeof(p->title) - 1) p->title[title_len++] = tc;
                }
            }
            i++;
            continue;
        }

        // Plain character data.
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n'){
            if (wlen){ emit_word(&L, word, wlen); wlen = 0; }
            if (L.sty.pre){
                // Preformatted text is the one place layout has to honour the
                // source's own spacing, so whitespace advances the pen instead
                // of being folded into a single collapsed gap.
                if (c == '\n') newline(&L, 0);
                else if (c == '\t') L.x += 4 * space_w(L.sty.face);
                else if (c != '\r') L.x += space_w(L.sty.face);
                if (!L.line_started && c != '\n'){
                    L.line_started = 1;
                    if (face_h(L.sty.face) > L.line_h) L.line_h = face_h(L.sty.face);
                    if (face_asc(L.sty.face) > L.line_asc) L.line_asc = face_asc(L.sty.face);
                }
            } else {
                L.pending_space = 1;
            }
            i++;
            continue;
        }

        if (!is_plain && c == '&'){
            char rep;
            uint32_t used = decode_entity(src + i, len - i, &rep);
            if (used){
                if (rep == ' '){
                    if (wlen){ emit_word(&L, word, wlen); wlen = 0; }
                    L.pending_space = 1;
                } else if (wlen < sizeof(word) - 1){
                    word[wlen++] = rep;
                }
                i += used;
                continue;
            }
        }

        if (c < 32 || c > 126) c = ' ';       // no glyph for it in the tables
        if (c == ' '){
            if (wlen){ emit_word(&L, word, wlen); wlen = 0; }
            L.pending_space = 1;
        } else if (wlen < sizeof(word) - 1){
            word[wlen++] = c;
        } else {
            emit_word(&L, word, wlen);
            wlen = 0;
            word[wlen++] = c;
        }
        i++;
    }

    if (wlen) emit_word(&L, word, wlen);
    if (L.line_started) align_line(&L);

    while (title_len && p->title[title_len - 1] == ' ') title_len--;

    // Anything still open at end of document gets closed here, so a page that
    // never closes its <blockquote> still gets the edge drawn.
    for (int d = L.depth - 1; d >= 0; d--) frame_close(&L, &L.stack[d]);
    L.depth = 0;

    p->title[title_len] = 0;

    // Link rectangles, built once from the finished runs. Doing it here rather
    // than incrementally is what lets align_line move runs after the fact: a
    // rect computed at emit time would be stale by a few pixels on every line
    // that mixed faces.
    for (uint32_t r = 0; r < p->run_n; r++){
        const html_run_t* run = &p->runs[r];
        if (run->link < 0 || (uint32_t)run->link >= p->link_n) continue;
        html_link_t* lk = &p->links[run->link];
        if (lk->w == 0 && lk->h == 0){
            lk->x = run->x; lk->y = run->y; lk->w = run->w; lk->h = run->h;
        } else {
            int x0 = lk->x < run->x ? lk->x : run->x;
            int y0 = lk->y < run->y ? lk->y : run->y;
            int x1 = (lk->x + lk->w) > (run->x + run->w) ? (lk->x + lk->w) : (run->x + run->w);
            int y1 = (lk->y + lk->h) > (run->y + run->h) ? (lk->y + lk->h) : (run->y + run->h);
            lk->x = x0; lk->y = y0; lk->w = x1 - x0; lk->h = y1 - y0;
        }
    }

    p->height = L.y + (L.line_h ? L.line_h : face_h(HTML_FACE_BODY)) + MARGIN;
    return p;
}

void html_free(html_page_t* p){
    if (!p) return;
    if (p->text)  kfree(p->text);
    if (p->runs)  kfree(p->runs);
    if (p->boxes) kfree(p->boxes);
    if (p->links) kfree(p->links);
    kfree(p);
}

void html_paint(const html_page_t* p, int vx, int vy, int vw, int vh, int scroll){
    if (!p) return;

    // Boxes first: they are the ground the text sits on. They are also the one
    // array not sorted by y, so this pass skips rather than stops.
    for (uint32_t i = 0; i < p->box_n; i++){
        const html_box_t* b = &p->boxes[i];
        int sy = vy + b->y - scroll;
        if (sy + b->h < vy || sy > vy + vh) continue;
        int sx = vx + b->x;

        switch (b->kind){
            case HTML_BOX_BULLET:
                gfx_fill_circle(sx + b->w / 2, sy + b->h / 2, b->w / 2, b->color);
                break;
            case HTML_BOX_FRAME:
                gfx_fill_rect((uint32_t)sx, (uint32_t)sy, (uint32_t)b->w, (uint32_t)b->h,
                              COL_FRAME_BG);
                gfx_draw_rect((uint32_t)sx, (uint32_t)sy, (uint32_t)b->w, (uint32_t)b->h,
                              b->color);
                break;
            default:
                gfx_fill_rect((uint32_t)sx, (uint32_t)sy, (uint32_t)b->w, (uint32_t)b->h,
                              b->color);
                break;
        }
    }

    for (uint32_t i = 0; i < p->run_n; i++){
        const html_run_t* r = &p->runs[i];
        int sy = vy + r->y - scroll;

        // Cheap vertical cull. Text runs are emitted in document order, so
        // once we are past the bottom of the viewport nothing later can be
        // inside it either.
        if (sy + r->h < vy) continue;
        if (sy > vy + vh) break;

        int sx = vx + r->x;
        const char* s = p->text + r->off;

        if (r->face == HTML_FACE_MONO)
            gfx_text_n((uint32_t)sx, (uint32_t)sy, s, r->len, r->color, GFX_TRANSPARENT);
        else
            gfx_pf_text_n((uint32_t)sx, (uint32_t)sy, s, r->len, pf_of(r->face), r->color);

        if (r->link >= 0 && !(r->flags & HTML_RUN_NO_UNDERLINE))
            gfx_hline((uint32_t)sx, (uint32_t)(sy + face_asc(r->face) + 2),
                      (uint32_t)r->w, r->color);
    }
    (void)vw;
}

int html_hit_link(const html_page_t* p, int px, int py){
    if (!p) return -1;
    for (uint32_t i = 0; i < p->run_n; i++){
        const html_run_t* r = &p->runs[i];
        if (r->link < 0) continue;
        if (px >= r->x && px < r->x + r->w && py >= r->y && py < r->y + r->h)
            return r->link;
    }
    return -1;
}

int html_link_href(const html_page_t* p, int idx, char* out, uint32_t cap){
    if (!p || idx < 0 || (uint32_t)idx >= p->link_n) return -1;
    const html_link_t* lk = &p->links[idx];
    uint32_t n = lk->href_len < cap - 1 ? lk->href_len : cap - 1;
    memcpy(out, p->text + lk->href_off, n);
    out[n] = 0;
    return 0;
}
