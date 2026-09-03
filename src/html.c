// src/html.c — HTML tokeniser and single-pass layout
//
// This is a renderer, not a browser engine. It walks the markup once with a
// small style stack and emits positioned text runs; there is no DOM, no CSS
// cascade and no reflow. What that buys is that a page costs one linear pass
// and a flat array, which is what makes it viable to render Wikipedia inside
// a 128 MB budget on a hobby kernel.
//
// What it does handle, because pages are unreadable without it: block-level
// breaks, headings, lists, links, entity decoding, and skipping the contents
// of <script> and <style> (which would otherwise dump source code into the
// middle of the text).
#include <stdint.h>
#include "header/html.h"
#include "header/gfx.h"
#include "header/wm.h"
#include "header/theme.h"
#include "header/kheap.h"
#include "header/kstring.h"
#include "header/font.h"

#define TEXT_CAP_INIT  (64u * 1024u)
#define RUN_CAP_INIT   4096u
#define LINK_CAP_INIT  512u

#define LINE_GAP       4
#define PARA_GAP       10
#define INDENT_STEP    24
#define MARGIN         12

// Pages are re-coloured for the dark theme rather than rendered on white.
// A browser that punched a bright rectangle into a dark desktop every time
// it loaded a page would be the one thing on screen you could not look at.
#define COL_BODY       GFX_RGB(0xD6, 0xDD, 0xE6)
#define COL_HEAD       GFX_RGB(0xFF, 0xFF, 0xFF)
#define COL_LINK       TH_LINK
#define COL_QUOTE      GFX_RGB(0x9A, 0xA6, 0xB4)

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
        // an 8x16 CP437-ish font, so it becomes '?' rather than a random one.
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

typedef struct {
    html_page_t* p;
    int   width;

    int   x, y;             // pen position
    int   line_h;           // tallest run on the current line
    int   indent;
    int   scale;
    uint32_t color;
    int   link;             // current link index, or -1
    int   pending_space;    // collapsed whitespace waiting to be emitted
    int   line_started;

    // Reader mode: while `skipping`, everything is discarded until the
    // matching close tag. Nesting is tracked by counting opens and closes of
    // the same tag name, which is enough for well-formed markup and cannot
    // run away on malformed markup because the counter only ever decreases
    // on a close.
    int   skipping;
    char  skip_tag[16];
    int   skip_nest;
} lay_t;

static void newline(lay_t* L, int extra){
    if (L->line_started || extra){
        L->y += (L->line_h ? L->line_h : FONT_H) + LINE_GAP + extra;
    }
    L->x = MARGIN + L->indent;
    L->line_h = 0;
    L->line_started = 0;
    L->pending_space = 0;
}

// Ends the current line only if something is on it, so consecutive block
// tags (</p><div>) do not stack up blank lines.
static void block_break(lay_t* L, int extra){
    if (L->line_started) newline(L, extra);
    else if (extra) L->y += extra;
}

static void emit_word(lay_t* L, const char* word, uint32_t len){
    if (!len) return;

    int cw = FONT_W * L->scale;
    int ch = FONT_H * L->scale;
    int space = (L->pending_space && L->line_started) ? cw : 0;
    int wpx = (int)len * cw;

    if (L->line_started && L->x + space + wpx > L->width - MARGIN){
        newline(L, 0);
        space = 0;
    }
    // A single word longer than the viewport still has to go somewhere; let
    // it overflow its line rather than looping forever trying to fit it.
    L->x += space;

    if (runs_grow(L->p) != 0) return;
    uint32_t off = arena_put(L->p, word, len);
    if (off == 0xFFFFFFFFu) return;

    html_run_t* r = &L->p->runs[L->p->run_n++];
    r->x = L->x;  r->y = L->y;
    r->w = wpx;   r->h = ch;
    r->scale = (uint8_t)L->scale;
    r->link  = L->link;
    r->color = L->color;
    r->off = off;
    r->len = len;

    if (L->link >= 0 && (uint32_t)L->link < L->p->link_n){
        // Grow the link's hit rectangle to cover every run it spans, so a
        // link that wraps is clickable on both lines.
        html_link_t* lk = &L->p->links[L->link];
        if (lk->w == 0 && lk->h == 0){ lk->x = L->x; lk->y = L->y; lk->w = wpx; lk->h = ch; }
        else {
            int x0 = lk->x < L->x ? lk->x : L->x;
            int y0 = lk->y < L->y ? lk->y : L->y;
            int x1 = (lk->x + lk->w) > (L->x + wpx) ? (lk->x + lk->w) : (L->x + wpx);
            int y1 = (lk->y + lk->h) > (L->y + ch) ? (lk->y + lk->h) : (L->y + ch);
            lk->x = x0; lk->y = y0; lk->w = x1 - x0; lk->h = y1 - y0;
        }
    }

    L->x += wpx;
    if (ch > L->line_h) L->line_h = ch;
    L->line_started = 1;
    L->pending_space = 0;
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
static int is_chrome_attr(const char* attrs, uint32_t alen){
    static const char* const C[] = {
        "mw-portlet", "vector-dropdown", "vector-menu", "vector-toc",
        "vector-page-tools", "vector-header", "vector-sticky",
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

// ---------------------------------------------------------------- entry

html_page_t* html_layout(const char* src, uint32_t len, int width, int is_plain){
    html_page_t* p = (html_page_t*)kmalloc(sizeof(html_page_t));
    if (!p) return 0;
    memset(p, 0, sizeof(*p));

    lay_t L;
    memset(&L, 0, sizeof(L));
    L.p = p;
    L.width = width;
    L.x = MARGIN;
    L.y = MARGIN;
    L.scale = 1;
    L.color = COL_BODY;
    L.link = -1;

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

            // <script> and <style> hold code, not prose. Skip to the matching
            // close tag rather than rendering their contents as text.
            if (!closing && (tag_is(tag, nlen, "script") || tag_is(tag, nlen, "style"))){
                const char* close = tag_is(tag, nlen, "script") ? "</script" : "</style";
                uint32_t clen = strlen(close);
                uint32_t j = te;
                while (j + clen < len && kstrnicmp(src + j, close, clen) != 0) j++;
                while (j < len && src[j] != '>') j++;
                i = (j < len) ? j + 1 : len;
                continue;
            }

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

            if (tag_is(tag, nlen, "a")){
                if (closing){
                    L.link = -1;
                    L.color = COL_BODY;
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
                                L.link  = (int)p->link_n++;
                                L.color = COL_LINK;
                            }
                        }
                    }
                }
                i = (te < len) ? te + 1 : len;
                continue;
            }

            if (tag_is(tag, nlen, "h1") || tag_is(tag, nlen, "h2")){
                block_break(&L, PARA_GAP);
                L.scale = closing ? 1 : 2;
                L.color = closing ? COL_BODY : COL_HEAD;
                if (closing) block_break(&L, PARA_GAP);
                i = (te < len) ? te + 1 : len;
                continue;
            }
            if (tag_is(tag, nlen, "h3") || tag_is(tag, nlen, "h4")
             || tag_is(tag, nlen, "h5") || tag_is(tag, nlen, "h6")){
                block_break(&L, PARA_GAP);
                L.color = closing ? COL_BODY : COL_HEAD;
                i = (te < len) ? te + 1 : len;
                continue;
            }

            if (tag_is(tag, nlen, "li")){
                if (!closing){
                    block_break(&L, 0);
                    emit_word(&L, "-", 1);
                    L.pending_space = 1;
                }
                i = (te < len) ? te + 1 : len;
                continue;
            }

            if (tag_is(tag, nlen, "ul") || tag_is(tag, nlen, "ol")){
                block_break(&L, 0);
                L.indent += closing ? -INDENT_STEP : INDENT_STEP;
                if (L.indent < 0) L.indent = 0;
                L.x = MARGIN + L.indent;
                i = (te < len) ? te + 1 : len;
                continue;
            }

            if (tag_is(tag, nlen, "blockquote")){
                block_break(&L, PARA_GAP);
                L.indent += closing ? -INDENT_STEP : INDENT_STEP;
                if (L.indent < 0) L.indent = 0;
                L.color = closing ? COL_BODY : COL_QUOTE;
                L.x = MARGIN + L.indent;
                i = (te < len) ? te + 1 : len;
                continue;
            }

            if (tag_is(tag, nlen, "br")){ newline(&L, 0); i = (te < len) ? te + 1 : len; continue; }

            if (tag_is(tag, nlen, "hr")){
                block_break(&L, PARA_GAP);
                i = (te < len) ? te + 1 : len;
                continue;
            }

            if (is_block_tag(tag, nlen)) block_break(&L, tag_is(tag, nlen, "p") ? PARA_GAP : 0);

            i = (te < len) ? te + 1 : len;
            continue;
        }

        if (L.skipping){ i++; continue; }

        // Plain character data.
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n'){
            if (wlen){ emit_word(&L, word, wlen); wlen = 0; }
            if (is_plain && c == '\n') newline(&L, 0);
            else L.pending_space = 1;
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

        if (in_title){
            if (title_len < sizeof(p->title) - 1 && (c >= 32 && c < 127))
                p->title[title_len++] = c;
            i++;
            continue;
        }

        if (c < 32 || c > 126) c = ' ';       // no glyph for it in this font
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
    p->title[title_len] = 0;

    p->height = L.y + (L.line_h ? L.line_h : FONT_H) + MARGIN;
    return p;
}

void html_free(html_page_t* p){
    if (!p) return;
    if (p->text)  kfree(p->text);
    if (p->runs)  kfree(p->runs);
    if (p->links) kfree(p->links);
    kfree(p);
}

void html_paint(const html_page_t* p, int vx, int vy, int vw, int vh, int scroll){
    if (!p) return;

    for (uint32_t i = 0; i < p->run_n; i++){
        const html_run_t* r = &p->runs[i];
        int sy = vy + r->y - scroll;

        // Cheap vertical cull. Runs are emitted in document order, so once
        // we are past the bottom of the viewport nothing later can be inside
        // it either.
        if (sy + r->h < vy) continue;
        if (sy > vy + vh) break;

        int sx = vx + r->x;
        const char* s = p->text + r->off;

        if (r->scale > 1) gfx_text_scaled((uint32_t)sx, (uint32_t)sy, s, r->color, GFX_TRANSPARENT, r->scale);
        else              gfx_text_n((uint32_t)sx, (uint32_t)sy, s, r->len, r->color, GFX_TRANSPARENT);

        if (r->link >= 0)
            gfx_hline((uint32_t)sx, (uint32_t)(sy + r->h - 1), (uint32_t)r->w, r->color);
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
