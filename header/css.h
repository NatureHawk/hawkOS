#pragma once
#include <stdint.h>

// A deliberately small CSS subset.
//
// This is not an attempt at a cascade. There is no specificity ordering, no
// inheritance model beyond what the layout's style stack already gives, no
// box model, and no computed values — a real implementation of those needs a
// DOM, and the whole design of this renderer is that there isn't one.
//
// What it does implement is the handful of declarations that decide whether a
// page is readable: which parts of it are hidden, what colour and weight the
// text is, how big a heading is, and where a block sits. That covers most of
// what a modern page's stylesheet is actually saying about its content, and
// it slots into the layout's existing style stack rather than replacing it.
//
// Selectors: a tag name, .class, #id, or a comma-separated list of those.
// A rule matches an element if any of its selectors does. Descendant and
// child combinators are parsed and then ignored — matching only the rightmost
// simple selector — because getting them right needs the ancestor chain, and
// over-applying a rule is much less damaging than dropping the whole sheet.

#define CSS_MAX_RULES  192
#define CSS_SEL_MAX    40

// Which fields of css_props_t actually carry a value. A stylesheet that says
// nothing about colour must leave the inherited colour alone, so "unset" has
// to be distinguishable from "black".
enum {
    CSS_HAS_COLOR      = 1u << 0,
    CSS_HAS_BG         = 1u << 1,
    CSS_HAS_WEIGHT     = 1u << 2,
    CSS_HAS_ITALIC     = 1u << 3,
    CSS_HAS_SIZE       = 1u << 4,
    CSS_HAS_ALIGN      = 1u << 5,
    CSS_HAS_DISPLAY    = 1u << 6,
    CSS_HAS_INDENT     = 1u << 7,
    CSS_HAS_MONO       = 1u << 8,
    CSS_HAS_UNDERLINE  = 1u << 9
};

enum { CSS_ALIGN_LEFT = 0, CSS_ALIGN_CENTER, CSS_ALIGN_RIGHT };

typedef struct {
    uint32_t have;          // bitmask of CSS_HAS_*
    uint32_t color;
    uint32_t bg;
    uint8_t  bold;
    uint8_t  italic;
    uint8_t  mono;
    uint8_t  align;
    uint8_t  hidden;        // display: none
    uint8_t  underline;     // text-decoration; 0 = none
    int16_t  size_px;       // resolved font-size in pixels
    int16_t  indent;        // margin-left + padding-left, in pixels
} css_props_t;

typedef struct {
    char        sel[CSS_SEL_MAX];   // one simple selector, lower-cased
    css_props_t props;
} css_rule_t;

typedef struct {
    css_rule_t rules[CSS_MAX_RULES];
    int        n;
    int        overflowed;          // more rules than the sheet can hold
} css_sheet_t;

void css_sheet_reset(css_sheet_t* s);

// Parses the contents of one <style> element and appends its rules.
void css_sheet_add(css_sheet_t* s, const char* text, uint32_t len);

// Parses a style="..." attribute value.
void css_parse_inline(const char* text, uint32_t len, css_props_t* out);

// Accumulates every rule matching this element into `out`, in sheet order, so
// a later rule wins. `class_attr` and `id_attr` may be empty strings.
void css_match(const css_sheet_t* s, const char* tag, uint32_t tag_len,
               const char* class_attr, const char* id_attr, css_props_t* out);

// Merges `src` over `dst`, field by field, for the fields src actually sets.
void css_merge(css_props_t* dst, const css_props_t* src);
