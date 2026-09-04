// src/css.c — the CSS subset described in header/css.h
//
// Everything here is a single linear pass over the text with no allocation.
// A stylesheet on a large page is a few hundred kilobytes of rules that mostly
// concern things this renderer has no concept of, so the parser's main job is
// to skip confidently: an at-rule, a media query, a selector it cannot match
// or a property it does not know all have to be stepped over without losing
// track of where the next rule begins.
#include <stdint.h>
#include "header/css.h"
#include "header/kstring.h"
#include "header/gfx.h"

static int is_space(char c){ return c==' '||c=='\t'||c=='\r'||c=='\n'||c=='\f'; }
static char lower(char c){ return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

void css_sheet_reset(css_sheet_t* s){ s->n = 0; s->overflowed = 0; }

// ------------------------------------------------------------------ values

static int hexval(char c){
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

typedef struct { const char* name; uint32_t rgb; } named_color_t;

// The colours a page is most likely to name in a way that matters. Anything
// else falls through and the inherited colour stands, which is the right
// failure: an unreadable page is worse than one that ignored a colour.
static const named_color_t NAMED[] = {
    { "black",   GFX_RGB(0x00,0x00,0x00) }, { "white",   GFX_RGB(0xFF,0xFF,0xFF) },
    { "red",     GFX_RGB(0xE0,0x60,0x60) }, { "green",   GFX_RGB(0x4C,0xC3,0x8A) },
    { "blue",    GFX_RGB(0x6F,0xB5,0xF0) }, { "yellow",  GFX_RGB(0xF5,0xC5,0x42) },
    { "orange",  GFX_RGB(0xE0,0xA0,0x36) }, { "purple",  GFX_RGB(0xB0,0x8C,0xE8) },
    { "gray",    GFX_RGB(0x8A,0x97,0xA7) }, { "grey",    GFX_RGB(0x8A,0x97,0xA7) },
    { "silver",  GFX_RGB(0xC0,0xC8,0xD0) }, { "maroon",  GFX_RGB(0xC0,0x50,0x50) },
    { "navy",    GFX_RGB(0x50,0x70,0xC0) }, { "teal",    GFX_RGB(0x40,0xB0,0xB0) },
    { "olive",   GFX_RGB(0xA0,0xA0,0x50) }, { "lime",    GFX_RGB(0x70,0xE0,0x70) },
    { "aqua",    GFX_RGB(0x70,0xE0,0xE0) }, { "cyan",    GFX_RGB(0x70,0xE0,0xE0) },
    { "fuchsia", GFX_RGB(0xE0,0x70,0xE0) }, { "magenta", GFX_RGB(0xE0,0x70,0xE0) },
};
#define NAMED_N ((int)(sizeof(NAMED)/sizeof(NAMED[0])))

// Dark theme, light text. A page asking for near-black text would be invisible
// against this background, so very dark colours are lifted rather than obeyed.
// The alternative — honouring them exactly — renders a great many pages as
// blank rectangles.
static uint32_t readable(uint32_t c){
    uint32_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    uint32_t lum = (r * 30 + g * 59 + b * 11) / 100;
    if (lum >= 90) return c;

    // Preserve the hue, raise the level to something legible.
    if (lum == 0) return GFX_RGB(0xD6, 0xDD, 0xE6);
    uint32_t scale = (120 * 256) / (lum ? lum : 1);
    r = (r * scale) >> 8; g = (g * scale) >> 8; b = (b * scale) >> 8;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return GFX_RGB(r, g, b);
}

// Parses a colour value. Returns 1 and writes *out on success.
static int parse_color(const char* v, uint32_t len, uint32_t* out){
    uint32_t i = 0;
    while (i < len && is_space(v[i])) i++;
    if (i >= len) return 0;

    if (v[i] == '#'){
        i++;
        int d[8], n = 0;
        while (i < len && n < 8){
            int h = hexval(v[i]);
            if (h < 0) break;
            d[n++] = h; i++;
        }
        if (n >= 6) *out = GFX_RGB(d[0]*16+d[1], d[2]*16+d[3], d[4]*16+d[5]);
        else if (n >= 3) *out = GFX_RGB(d[0]*17, d[1]*17, d[2]*17);
        else return 0;
        *out = readable(*out);
        return 1;
    }

    if (kstrnicmp(v + i, "rgb", 3) == 0){
        while (i < len && v[i] != '(') i++;
        i++;
        uint32_t comp[3] = {0,0,0};
        for (int c = 0; c < 3; c++){
            while (i < len && (is_space(v[i]) || v[i] == ',')) i++;
            uint32_t val = 0; int any = 0;
            while (i < len && v[i] >= '0' && v[i] <= '9'){ val = val*10 + (uint32_t)(v[i]-'0'); i++; any = 1; }
            if (!any) return 0;
            comp[c] = val > 255 ? 255 : val;
        }
        *out = readable(GFX_RGB(comp[0], comp[1], comp[2]));
        return 1;
    }

    for (int k = 0; k < NAMED_N; k++){
        uint32_t nl = strlen(NAMED[k].name);
        if (len - i >= nl && kstrnicmp(v + i, NAMED[k].name, nl) == 0){
            *out = readable(NAMED[k].rgb);
            return 1;
        }
    }
    return 0;
}

// Resolves a length to pixels against a 17px body. Relative units are taken
// against the body size rather than the parent's, which is wrong in general
// and close enough for the one thing sizes are used for here: choosing which
// of five faces to set the text in.
static int parse_len(const char* v, uint32_t len, int* out){
    uint32_t i = 0;
    while (i < len && is_space(v[i])) i++;

    int neg = 0;
    if (i < len && (v[i] == '-' || v[i] == '+')){ neg = (v[i] == '-'); i++; }

    int whole = 0, frac = 0, fdiv = 1, any = 0;
    while (i < len && v[i] >= '0' && v[i] <= '9'){ whole = whole*10 + (v[i]-'0'); i++; any = 1; }
    if (i < len && v[i] == '.'){
        i++;
        while (i < len && v[i] >= '0' && v[i] <= '9' && fdiv < 100){
            frac = frac*10 + (v[i]-'0'); fdiv *= 10; i++; any = 1;
        }
    }
    if (!any){
        // Keywords, for font-size.
        if (kstrnicmp(v, "small", 5) == 0)   { *out = 14; return 1; }
        if (kstrnicmp(v, "medium", 6) == 0)  { *out = 17; return 1; }
        if (kstrnicmp(v, "large", 5) == 0)   { *out = 21; return 1; }
        if (kstrnicmp(v, "x-large", 7) == 0) { *out = 26; return 1; }
        return 0;
    }

    int milli = whole * 1000 + (frac * 1000) / fdiv;   // value * 1000

    int px;
    if      (i + 1 < len && kstrnicmp(v+i, "px", 2) == 0) px = milli / 1000;
    else if (i + 1 < len && kstrnicmp(v+i, "pt", 2) == 0) px = (milli * 4) / 3000;
    else if (i + 1 < len && kstrnicmp(v+i, "em", 2) == 0) px = (milli * 17) / 1000;
    else if (i + 2 < len && kstrnicmp(v+i, "rem", 3) == 0) px = (milli * 17) / 1000;
    else if (i     < len && v[i] == '%')                  px = (milli * 17) / 100000;
    else px = milli / 1000;      // unitless: treat as px

    *out = neg ? -px : px;
    return 1;
}

// ------------------------------------------------------------- declarations

static void apply_decl(css_props_t* p, const char* name, uint32_t nlen,
                       const char* val, uint32_t vlen){
    #define IS(s) (nlen == strlen(s) && kstrnicmp(name, s, nlen) == 0)
    #define VAL_IS(s) (vlen >= strlen(s) && kstrnicmp(val, s, strlen(s)) == 0)

    if (IS("display")){
        if (VAL_IS("none")){ p->hidden = 1; p->have |= CSS_HAS_DISPLAY; }
        return;
    }
    if (IS("visibility")){
        if (VAL_IS("hidden")){ p->hidden = 1; p->have |= CSS_HAS_DISPLAY; }
        return;
    }
    if (IS("color")){
        uint32_t c;
        if (parse_color(val, vlen, &c)){ p->color = c; p->have |= CSS_HAS_COLOR; }
        return;
    }
    if (IS("background-color") || IS("background")){
        uint32_t c;
        // "background" is shorthand and may hold an image or gradient; take a
        // colour out of it only when that is plainly what it is.
        if (parse_color(val, vlen, &c)){ p->bg = c; p->have |= CSS_HAS_BG; }
        return;
    }
    if (IS("font-weight")){
        int bold = VAL_IS("bold") || VAL_IS("bolder")
                || VAL_IS("600") || VAL_IS("700") || VAL_IS("800") || VAL_IS("900");
        p->bold = (uint8_t)bold;
        p->have |= CSS_HAS_WEIGHT;
        return;
    }
    if (IS("font-style")){
        p->italic = (uint8_t)(VAL_IS("italic") || VAL_IS("oblique"));
        p->have |= CSS_HAS_ITALIC;
        return;
    }
    if (IS("font-family")){
        // Only the distinction the renderer can act on: is this monospace.
        p->mono = 0;
        for (uint32_t i = 0; i + 8 <= vlen; i++)
            if (kstrnicmp(val + i, "monospace", 9) == 0) { p->mono = 1; break; }
        if (p->mono) p->have |= CSS_HAS_MONO;
        return;
    }
    if (IS("font-size")){
        int px;
        if (parse_len(val, vlen, &px) && px > 4 && px < 120){
            p->size_px = (int16_t)px;
            p->have |= CSS_HAS_SIZE;
        }
        return;
    }
    if (IS("text-align")){
        if      (VAL_IS("center")) { p->align = CSS_ALIGN_CENTER; p->have |= CSS_HAS_ALIGN; }
        else if (VAL_IS("right"))  { p->align = CSS_ALIGN_RIGHT;  p->have |= CSS_HAS_ALIGN; }
        else if (VAL_IS("left"))   { p->align = CSS_ALIGN_LEFT;   p->have |= CSS_HAS_ALIGN; }
        return;
    }
    if (IS("text-decoration") || IS("text-decoration-line")){
        p->underline = (uint8_t)(!VAL_IS("none"));
        p->have |= CSS_HAS_UNDERLINE;
        return;
    }
    if (IS("margin-left") || IS("padding-left")){
        int px;
        if (parse_len(val, vlen, &px)){
            // Accumulate: margin and padding both push the content right.
            int cur = (p->have & CSS_HAS_INDENT) ? p->indent : 0;
            int total = cur + px;
            if (total < 0)   total = 0;
            if (total > 160) total = 160;   // a runaway indent leaves no measure
            p->indent = (int16_t)total;
            p->have |= CSS_HAS_INDENT;
        }
        return;
    }

    #undef IS
    #undef VAL_IS
}

// Parses "a: b; c: d" into props.
static void parse_decls(const char* t, uint32_t len, css_props_t* out){
    uint32_t i = 0;
    while (i < len){
        while (i < len && (is_space(t[i]) || t[i] == ';')) i++;
        uint32_t ns = i;
        while (i < len && t[i] != ':' && t[i] != ';' && t[i] != '}') i++;
        if (i >= len || t[i] != ':'){
            while (i < len && t[i] != ';') i++;
            continue;
        }
        uint32_t ne = i;
        while (ne > ns && is_space(t[ne-1])) ne--;
        i++;                                   // past ':'

        while (i < len && is_space(t[i])) i++;
        uint32_t vs = i;
        while (i < len && t[i] != ';' && t[i] != '}') i++;
        uint32_t ve = i;
        while (ve > vs && is_space(t[ve-1])) ve--;

        if (ne > ns && ve > vs) apply_decl(out, t + ns, ne - ns, t + vs, ve - vs);
    }
}

void css_parse_inline(const char* text, uint32_t len, css_props_t* out){
    parse_decls(text, len, out);
}

void css_merge(css_props_t* dst, const css_props_t* src){
    if (src->have & CSS_HAS_COLOR)     { dst->color = src->color; }
    if (src->have & CSS_HAS_BG)        { dst->bg = src->bg; }
    if (src->have & CSS_HAS_WEIGHT)    { dst->bold = src->bold; }
    if (src->have & CSS_HAS_ITALIC)    { dst->italic = src->italic; }
    if (src->have & CSS_HAS_MONO)      { dst->mono = src->mono; }
    if (src->have & CSS_HAS_SIZE)      { dst->size_px = src->size_px; }
    if (src->have & CSS_HAS_ALIGN)     { dst->align = src->align; }
    if (src->have & CSS_HAS_DISPLAY)   { dst->hidden = src->hidden; }
    if (src->have & CSS_HAS_INDENT)    { dst->indent = src->indent; }
    if (src->have & CSS_HAS_UNDERLINE) { dst->underline = src->underline; }
    dst->have |= src->have;
}

// ----------------------------------------------------------------- selectors

// Copies the rightmost simple selector, lower-cased, dropping pseudo-classes
// and attribute selectors. "article.main > p:first-child" becomes "p".
static void simplify_selector(const char* s, uint32_t len, char* out, uint32_t cap){
    // Trim first. The selector text runs up to the '{', so it almost always
    // ends in whitespace -- and without this the scan below treats that as a
    // combinator and decides the rightmost component is the empty string,
    // which silently drops every rule in the sheet.
    while (len && is_space(s[0])){ s++; len--; }
    while (len && is_space(s[len - 1])) len--;

    // Rightmost component: everything after the last space or combinator.
    uint32_t start = 0;
    for (uint32_t i = 0; i < len; i++)
        if (s[i] == ' ' || s[i] == '>' || s[i] == '+' || s[i] == '~' || s[i] == '\t' || s[i] == '\n')
            start = i + 1;

    // And within that, stop at the first pseudo or attribute marker.
    uint32_t o = 0;
    for (uint32_t i = start; i < len && o + 1 < cap; i++){
        char c = s[i];
        if (c == ':' || c == '[' || c == '(') break;
        // A compound like "div.cls" keeps only its last part: matching on the
        // class alone over-applies, which is the safer direction.
        if ((c == '.' || c == '#') && o > 0) o = 0;
        out[o++] = lower(c);
    }
    while (o > 0 && is_space(out[o-1])) o--;
    out[o] = 0;
}

static void add_rule(css_sheet_t* s, const char* sel, uint32_t sel_len,
                     const css_props_t* props){
    if (!props->have) return;
    if (s->n >= CSS_MAX_RULES){ s->overflowed = 1; return; }

    char simple[CSS_SEL_MAX];
    simplify_selector(sel, sel_len, simple, sizeof(simple));
    if (!simple[0] || simple[0] == '*') return;

    css_rule_t* r = &s->rules[s->n++];
    strncpy(r->sel, simple, CSS_SEL_MAX - 1);
    r->sel[CSS_SEL_MAX - 1] = 0;
    r->props = *props;
}

void css_sheet_add(css_sheet_t* s, const char* text, uint32_t len){
    uint32_t i = 0;

    while (i < len){
        while (i < len && is_space(text[i])) i++;
        if (i >= len) break;

        // Comments can appear between anything.
        if (i + 1 < len && text[i] == '/' && text[i+1] == '*'){
            i += 2;
            while (i + 1 < len && !(text[i] == '*' && text[i+1] == '/')) i++;
            i += 2;
            continue;
        }

        // At-rules. @media and @supports wrap ordinary rules in another set of
        // braces; descending into them rather than skipping is what makes a
        // responsive page render at all, since much of a modern sheet lives
        // inside one. Everything else (@import, @font-face, @keyframes) is
        // skipped whole.
        if (text[i] == '@'){
            uint32_t ns = i;
            while (i < len && text[i] != '{' && text[i] != ';') i++;
            int nested = (i - ns >= 6 && (kstrnicmp(text + ns, "@media", 6) == 0))
                      || (i - ns >= 9 && (kstrnicmp(text + ns, "@supports", 9) == 0));
            if (i < len && text[i] == ';'){ i++; continue; }
            if (i >= len) break;
            i++;                                  // past '{'
            if (nested) continue;                 // parse the rules inside

            // Skip the whole block, tracking nesting so an inner rule's brace
            // does not end it early.
            int depth = 1;
            while (i < len && depth){
                if (text[i] == '{') depth++;
                else if (text[i] == '}') depth--;
                i++;
            }
            continue;
        }

        if (text[i] == '}'){ i++; continue; }      // close of an @media block

        // selector-list '{' declarations '}'
        uint32_t sel_start = i;
        while (i < len && text[i] != '{' && text[i] != '}') i++;
        if (i >= len || text[i] != '{') break;
        uint32_t sel_end = i;
        i++;

        uint32_t decl_start = i;
        int depth = 1;
        while (i < len && depth){
            if (text[i] == '{') depth++;
            else if (text[i] == '}') depth--;
            if (depth) i++;
        }
        uint32_t decl_end = i;
        if (i < len) i++;                          // past '}'

        css_props_t props;
        memset(&props, 0, sizeof(props));
        parse_decls(text + decl_start, decl_end - decl_start, &props);
        if (!props.have) continue;

        // Split the selector list on commas; each gets its own rule.
        uint32_t p = sel_start;
        while (p < sel_end){
            uint32_t q = p;
            while (q < sel_end && text[q] != ',') q++;
            add_rule(s, text + p, q - p, &props);
            p = q + 1;
        }
    }
}

// ------------------------------------------------------------------ matching

// True if `list` (a space-separated class attribute) contains `want`.
static int class_list_has(const char* list, const char* want){
    uint32_t wl = strlen(want);
    if (!wl) return 0;
    uint32_t i = 0;
    while (list[i]){
        while (list[i] == ' ') i++;
        uint32_t s = i;
        while (list[i] && list[i] != ' ') i++;
        if (i - s == wl && kstrnicmp(list + s, want, wl) == 0) return 1;
    }
    return 0;
}

static int rule_matches(const css_rule_t* r, const char* tag, uint32_t tag_len,
                        const char* class_attr, const char* id_attr){
    if (r->sel[0] == '.') return class_list_has(class_attr, r->sel + 1);
    if (r->sel[0] == '#') return kstricmp(id_attr, r->sel + 1) == 0;

    uint32_t sl = strlen(r->sel);
    return sl == tag_len && kstrnicmp(tag, r->sel, sl) == 0;
}

void css_match(const css_sheet_t* s, const char* tag, uint32_t tag_len,
               const char* class_attr, const char* id_attr, css_props_t* out){
    for (int i = 0; i < s->n; i++)
        if (rule_matches(&s->rules[i], tag, tag_len, class_attr, id_attr))
            css_merge(out, &s->rules[i].props);
}
