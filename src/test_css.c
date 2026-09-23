// src/test_css.c — the CSS subset, and what it does to a laid-out page
//
// Two halves. The first tests the parser directly, because a value parser is
// exactly the kind of code that looks right and is wrong at the edges. The
// second goes through html_layout, because what actually matters is not that
// "color: red" parsed but that the run came out red.
#include "header/ktest.h"
#include "header/kstring.h"
#include "header/css.h"
#include "header/html.h"
#include "header/gfx.h"
#include "header/theme.h"

static void inl(const char* s, css_props_t* p){
    memset(p, 0, sizeof(*p));
    css_parse_inline(s, strlen(s), p);
}

// A parsed colour is pulled towards something legible on the background it
// will actually be drawn on, so which appearance is current changes the
// answer. Tests about colour therefore state the appearance they are about
// and put the system back afterwards, rather than inheriting whatever it
// happened to be in when the suite started.

// ------------------------------------------------------------- the parser

KTEST(css, inline_colors){
    css_props_t p;
    int was = theme_is_dark();
    theme_set_dark(1);              // white needs no adjusting on a dark page

    inl("color: #ffffff", &p);
    KT_TRUE(p.have & CSS_HAS_COLOR);
    KT_EQ(p.color, GFX_RGB(0xFF, 0xFF, 0xFF));

    // Three-digit hex expands each nibble.
    inl("color:#fff", &p);
    KT_EQ(p.color, GFX_RGB(0xFF, 0xFF, 0xFF));

    inl("color: rgb(255, 255, 255)", &p);
    KT_EQ(p.color, GFX_RGB(0xFF, 0xFF, 0xFF));

    inl("color: white", &p);
    KT_EQ(p.color, GFX_RGB(0xFF, 0xFF, 0xFF));

    theme_set_dark(was);
}

static uint32_t lum_of(uint32_t c){
    uint32_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    return (r * 30 + g * 59 + b * 11) / 100;
}

KTEST(css, colors_are_pulled_towards_readable){
    css_props_t p;
    int was = theme_is_dark();

    // Dark appearance: black text would be invisible, so it comes back as
    // something legible rather than being obeyed exactly, and a colour that
    // is already light is left alone.
    theme_set_dark(1);
    inl("color: #000000", &p);
    KT_TRUE(p.have & CSS_HAS_COLOR);
    KT_TRUE(lum_of(p.color) >= 90);

    inl("color: #e0e8f0", &p);
    KT_EQ(p.color, GFX_RGB(0xE0, 0xE8, 0xF0));

    // Light appearance: the failure runs the other way. A page that asks for
    // white is asking against its own dark background, and would vanish.
    theme_set_dark(0);
    inl("color: #ffffff", &p);
    KT_TRUE(lum_of(p.color) <= 165);

    inl("color: #1a1a1a", &p);
    KT_EQ(p.color, GFX_RGB(0x1A, 0x1A, 0x1A));

    theme_set_dark(was);
}

KTEST(css, unparseable_values_set_nothing){
    css_props_t p;
    inl("color: var(--brand)", &p);
    KT_FALSE(p.have & CSS_HAS_COLOR);

    inl("color:", &p);
    KT_EQ(p.have, 0);

    inl("nonsense", &p);
    KT_EQ(p.have, 0);

    // A property this renderer does not implement must not corrupt the ones
    // around it.
    inl("border-radius: 4px; color: white; z-index: 10", &p);
    KT_TRUE(p.have & CSS_HAS_COLOR);
}

KTEST(css, font_size_units){
    css_props_t p;

    inl("font-size: 24px", &p);
    KT_TRUE(p.have & CSS_HAS_SIZE);
    KT_EQ(p.size_px, 24);

    inl("font-size: 12pt", &p);       // 12pt = 16px
    KT_EQ(p.size_px, 16);

    inl("font-size: 2em", &p);        // against a 17px body
    KT_EQ(p.size_px, 34);

    inl("font-size: 1.5em", &p);
    KT_EQ(p.size_px, 25);

    inl("font-size: 200%", &p);
    KT_EQ(p.size_px, 34);

    inl("font-size: large", &p);
    KT_EQ(p.size_px, 21);
}

KTEST(css, weight_style_and_family){
    css_props_t p;

    inl("font-weight: bold", &p);
    KT_TRUE(p.have & CSS_HAS_WEIGHT);
    KT_EQ(p.bold, 1);

    inl("font-weight: 700", &p);
    KT_EQ(p.bold, 1);

    inl("font-weight: 400", &p);
    KT_TRUE(p.have & CSS_HAS_WEIGHT);
    KT_EQ(p.bold, 0);

    inl("font-style: italic", &p);
    KT_EQ(p.italic, 1);

    inl("font-family: 'Courier New', monospace", &p);
    KT_TRUE(p.have & CSS_HAS_MONO);
    KT_EQ(p.mono, 1);

    inl("font-family: Helvetica, Arial, sans-serif", &p);
    KT_FALSE(p.have & CSS_HAS_MONO);
}

KTEST(css, display_none_and_visibility){
    css_props_t p;
    inl("display: none", &p);
    KT_TRUE(p.have & CSS_HAS_DISPLAY);
    KT_EQ(p.hidden, 1);

    inl("visibility: hidden", &p);
    KT_EQ(p.hidden, 1);

    inl("display: block", &p);
    KT_EQ(p.hidden, 0);
}

KTEST(css, indent_accumulates_margin_and_padding){
    css_props_t p;
    inl("margin-left: 20px; padding-left: 10px", &p);
    KT_TRUE(p.have & CSS_HAS_INDENT);
    KT_EQ(p.indent, 30);

    // A runaway value must be clamped, or the measure disappears entirely.
    inl("margin-left: 9000px", &p);
    KT_EQ(p.indent, 160);
}

KTEST(css, sheet_tag_class_and_id_selectors){
    css_sheet_t s;
    css_sheet_reset(&s);
    css_sheet_add(&s,
        "p { color: white } "
        ".warn { font-weight: bold } "
        "#main { margin-left: 20px }", 0);
    // Length of 0 would parse nothing; pass the real length.
    css_sheet_reset(&s);
    const char* sheet = "p { color: white } .warn { font-weight: bold } #main { margin-left: 20px }";
    css_sheet_add(&s, sheet, strlen(sheet));
    KT_EQ(s.n, 3);

    css_props_t p;
    memset(&p, 0, sizeof(p));
    css_match(&s, "p", 1, "", "", &p);
    KT_TRUE(p.have & CSS_HAS_COLOR);

    memset(&p, 0, sizeof(p));
    css_match(&s, "div", 3, "warn", "", &p);
    KT_TRUE(p.have & CSS_HAS_WEIGHT);
    KT_EQ(p.bold, 1);

    memset(&p, 0, sizeof(p));
    css_match(&s, "div", 3, "", "main", &p);
    KT_TRUE(p.have & CSS_HAS_INDENT);

    // A class that only looks like a prefix of the selector must not match.
    memset(&p, 0, sizeof(p));
    css_match(&s, "div", 3, "warning", "", &p);
    KT_EQ(p.have, 0);
}

KTEST(css, selector_lists_and_comments){
    css_sheet_t s;
    css_sheet_reset(&s);
    const char* sheet =
        "/* a comment { with braces } */\n"
        "h1, h2, h3 { color: white }\n"
        "nav, .menu { display: none }\n";
    css_sheet_add(&s, sheet, strlen(sheet));
    KT_EQ(s.n, 5);

    css_props_t p;
    memset(&p, 0, sizeof(p));
    css_match(&s, "h2", 2, "", "", &p);
    KT_TRUE(p.have & CSS_HAS_COLOR);

    memset(&p, 0, sizeof(p));
    css_match(&s, "div", 3, "menu", "", &p);
    KT_EQ(p.hidden, 1);
}

KTEST(css, at_rules_are_skipped_but_media_is_entered){
    css_sheet_t s;
    css_sheet_reset(&s);
    const char* sheet =
        "@import url('x.css');\n"
        "@font-face { font-family: 'X'; src: url(y.woff) }\n"
        "@keyframes spin { from { color: red } to { color: blue } }\n"
        "@media screen and (min-width: 600px) { p { color: white } }\n"
        "b { font-weight: bold }\n";
    css_sheet_add(&s, sheet, strlen(sheet));

    css_props_t p;
    // Rules inside @media are ordinary rules and must be honoured; much of a
    // modern sheet lives inside one.
    memset(&p, 0, sizeof(p));
    css_match(&s, "p", 1, "", "", &p);
    KT_TRUE(p.have & CSS_HAS_COLOR);

    // And the sheet must not have lost its footing on the way past the
    // at-rules it skipped.
    memset(&p, 0, sizeof(p));
    css_match(&s, "b", 1, "", "", &p);
    KT_TRUE(p.have & CSS_HAS_WEIGHT);

    // @keyframes must not have contributed a "from" or "to" rule.
    memset(&p, 0, sizeof(p));
    css_match(&s, "from", 4, "", "", &p);
    KT_EQ(p.have, 0);
}

KTEST(css, later_rules_win){
    css_sheet_t s;
    css_sheet_reset(&s);
    // Both colours are mid-toned, so neither is adjusted for legibility in
    // either appearance. This test is about which rule wins, and picking a
    // value the palette would rewrite would make it about something else.
    const char* sheet = "p { color: red } p { color: green }";
    css_sheet_add(&s, sheet, strlen(sheet));

    css_props_t p;
    memset(&p, 0, sizeof(p));
    css_match(&s, "p", 1, "", "", &p);
    KT_EQ(p.color, GFX_RGB(0x4C, 0xC3, 0x8A));
}

KTEST(css, sheet_overflow_is_survivable){
    css_sheet_t s;
    css_sheet_reset(&s);
    // Far more rules than the sheet holds. It must stop taking them and say
    // so, not overrun the array.
    static char big[16384];
    uint32_t o = 0;
    for (int i = 0; i < 400 && o < sizeof(big) - 40; i++)
        o += (uint32_t)ksnprintf(big + o, sizeof(big) - o, "c%d { color: white }", i);
    css_sheet_add(&s, big, o);
    KT_TRUE(s.n <= CSS_MAX_RULES);
    KT_EQ(s.overflowed, 1);
}

// -------------------------------------------------- through the layout

static int count_face(const html_page_t* p, int face){
    int n = 0;
    for (uint32_t i = 0; i < p->run_n; i++) if (p->runs[i].face == face) n++;
    return n;
}

static void page_text(const html_page_t* p, char* out, uint32_t cap){
    uint32_t o = 0;
    out[0] = 0;
    for (uint32_t i = 0; i < p->run_n && o + 1 < cap; i++){
        const html_run_t* r = &p->runs[i];
        if (o && o + 1 < cap) out[o++] = ' ';
        for (uint32_t k = 0; k < r->len && o + 1 < cap; k++) out[o++] = p->text[r->off + k];
    }
    out[o] = 0;
}

KTEST(css, style_element_hides_a_subtree){
    // The payoff: the page's own stylesheet says what is furniture, so the
    // renderer no longer has to guess from a list of class names.
    const char* src =
        "<style>.sidebar { display: none }</style>"
        "<p>content</p>"
        "<div class=\"sidebar\"><p>menu one</p><p>menu two</p></div>"
        "<p>more content</p>";
    html_page_t* p = html_layout(src, strlen(src), 600, 0);
    KT_NOTNULL(p);
    char buf[128];
    page_text(p, buf, sizeof(buf));
    KT_STREQ(buf, "content more content");
    html_free(p);
}

KTEST(css, inline_style_sets_colour_and_weight){
    const char* src = "<p><span style=\"color:#ff0000\">red</span> "
                      "<span style=\"font-weight:bold\">bold</span></p>";
    html_page_t* p = html_layout(src, strlen(src), 600, 0);
    KT_NOTNULL(p);
    KT_EQ(count_face(p, HTML_FACE_BOLD), 1);

    int coloured = 0;
    for (uint32_t i = 0; i < p->run_n; i++)
        if (p->runs[i].color != GFX_RGB(0xD6, 0xDD, 0xE6)) coloured++;
    KT_TRUE(coloured >= 1);
    html_free(p);
}

KTEST(css, font_size_picks_a_heading_face){
    const char* src = "<style>.big { font-size: 28px }</style>"
                      "<div class=\"big\">huge</div><p>normal</p>";
    html_page_t* p = html_layout(src, strlen(src), 600, 0);
    KT_NOTNULL(p);
    KT_EQ(count_face(p, HTML_FACE_H1), 1);
    KT_EQ(count_face(p, HTML_FACE_BODY), 1);
    html_free(p);
}

KTEST(css, inline_style_beats_the_sheet){
    const char* src = "<style>p { font-weight: bold }</style>"
                      "<p style=\"font-weight: normal\">plain</p>";
    html_page_t* p = html_layout(src, strlen(src), 600, 0);
    KT_NOTNULL(p);
    KT_EQ(count_face(p, HTML_FACE_BOLD), 0);
    KT_EQ(count_face(p, HTML_FACE_BODY), 1);
    html_free(p);
}

KTEST(css, style_scope_ends_with_the_element){
    // The style stack has to unwind: text after </div> must not inherit what
    // the div asked for.
    const char* src = "<div style=\"font-weight:bold\">inside</div><p>outside</p>";
    html_page_t* p = html_layout(src, strlen(src), 600, 0);
    KT_NOTNULL(p);
    KT_EQ(count_face(p, HTML_FACE_BOLD), 1);
    KT_EQ(count_face(p, HTML_FACE_BODY), 1);
    html_free(p);
}

KTEST(css, text_align_centre_shifts_the_line){
    const char* src_l = "<p>centred text</p>";
    const char* src_c = "<p style=\"text-align:center\">centred text</p>";
    html_page_t* l = html_layout(src_l, strlen(src_l), 600, 0);
    html_page_t* c = html_layout(src_c, strlen(src_c), 600, 0);
    KT_NOTNULL(l);
    KT_NOTNULL(c);
    KT_TRUE(l->run_n > 0);
    KT_TRUE(c->run_n > 0);
    if (l->run_n && c->run_n) KT_TRUE(c->runs[0].x > l->runs[0].x);
    html_free(l);
    html_free(c);
}

KTEST(css, text_decoration_none_suppresses_the_underline){
    const char* src = "<a href=\"/x\" style=\"text-decoration:none\">link</a>";
    html_page_t* p = html_layout(src, strlen(src), 600, 0);
    KT_NOTNULL(p);
    KT_TRUE(p->run_n > 0);
    if (p->run_n) KT_TRUE(p->runs[0].flags & HTML_RUN_NO_UNDERLINE);
    html_free(p);

    const char* plain = "<a href=\"/x\">link</a>";
    html_page_t* q = html_layout(plain, strlen(plain), 600, 0);
    KT_NOTNULL(q);
    if (q->run_n) KT_FALSE(q->runs[0].flags & HTML_RUN_NO_UNDERLINE);
    html_free(q);
}

KTEST(css, malformed_sheet_does_not_break_the_page){
    // Unterminated rule, unbalanced braces, a stray at-rule. The document
    // still has to render.
    const char* src =
        "<style>p { color: white ; .x { } @media { p {</style>"
        "<p>survivor</p>";
    html_page_t* p = html_layout(src, strlen(src), 600, 0);
    KT_NOTNULL(p);
    char buf[64];
    page_text(p, buf, sizeof(buf));
    KT_STREQ(buf, "survivor");
    html_free(p);
}
