// src/test_html.c — the HTML tokeniser and layout pass
//
// Layout is checked through the display list rather than through pixels: a
// screenshot tells you something changed, a run's x/y/face tells you what.
// The helpers below flatten a laid-out page back into text so a test can say
// what it means without hard-coding coordinates that any spacing tweak would
// break.
#include "header/ktest.h"
#include "header/kstring.h"
#include "header/html.h"

// Concatenates every run's text with single spaces, in document order.
static void page_text(const html_page_t* p, char* out, uint32_t cap){
    uint32_t o = 0;
    out[0] = 0;
    for (uint32_t i = 0; i < p->run_n && o + 1 < cap; i++){
        const html_run_t* r = &p->runs[i];
        if (o && o + 1 < cap) out[o++] = ' ';
        for (uint32_t k = 0; k < r->len && o + 1 < cap; k++)
            out[o++] = p->text[r->off + k];
    }
    out[o] = 0;
}

static int count_boxes(const html_page_t* p, int kind){
    int n = 0;
    for (uint32_t i = 0; i < p->box_n; i++) if (p->boxes[i].kind == kind) n++;
    return n;
}

static int count_face(const html_page_t* p, int face){
    int n = 0;
    for (uint32_t i = 0; i < p->run_n; i++) if (p->runs[i].face == face) n++;
    return n;
}

KTEST(html, plain_paragraph){
    html_page_t* p = html_layout("<p>hello world</p>", 18, 600, 0);
    KT_NOTNULL(p);
    char buf[128];
    page_text(p, buf, sizeof(buf));
    KT_STREQ(buf, "hello world");
    KT_TRUE(p->height > 0);
    html_free(p);
}

KTEST(html, whitespace_is_collapsed){
    const char* src = "<p>a   b\n\n\tc</p>";
    html_page_t* p = html_layout(src, strlen(src), 600, 0);
    KT_NOTNULL(p);
    char buf[64];
    page_text(p, buf, sizeof(buf));
    KT_STREQ(buf, "a b c");
    html_free(p);
}

KTEST(html, entities_decode){
    const char* src = "<p>a&amp;b &lt;tag&gt; &quot;q&quot; &#65;&#x42;</p>";
    html_page_t* p = html_layout(src, strlen(src), 600, 0);
    KT_NOTNULL(p);
    char buf[128];
    page_text(p, buf, sizeof(buf));
    KT_STREQ(buf, "a&b <tag> \"q\" AB");
    html_free(p);
}

KTEST(html, script_and_style_contents_are_dropped){
    const char* src = "<p>before</p><script>var x = 1;</script>"
                      "<style>p{color:red}</style><p>after</p>";
    html_page_t* p = html_layout(src, strlen(src), 600, 0);
    KT_NOTNULL(p);
    char buf[128];
    page_text(p, buf, sizeof(buf));
    KT_STREQ(buf, "before after");
    html_free(p);
}

KTEST(html, title_is_extracted_and_not_rendered){
    const char* src = "<html><head><title>My Page</title></head><body><p>body</p></body></html>";
    html_page_t* p = html_layout(src, strlen(src), 600, 0);
    KT_NOTNULL(p);
    KT_STREQ(p->title, "My Page");
    char buf[64];
    page_text(p, buf, sizeof(buf));
    KT_STREQ(buf, "body");
    html_free(p);
}

KTEST(html, links_are_collected_and_hit_testable){
    const char* src = "<p>go <a href=\"https://example.com/x\">there</a> now</p>";
    html_page_t* p = html_layout(src, strlen(src), 600, 0);
    KT_NOTNULL(p);
    KT_EQ(p->link_n, 1);

    char href[URL_MAX];
    KT_EQ(html_link_href(p, 0, href, sizeof(href)), 0);
    KT_STREQ(href, "https://example.com/x");

    // Find the run carrying the link and hit-test its own centre.
    int found = -1;
    for (uint32_t i = 0; i < p->run_n; i++) if (p->runs[i].link == 0) found = (int)i;
    KT_TRUE(found >= 0);
    if (found >= 0){
        const html_run_t* r = &p->runs[found];
        KT_EQ(html_hit_link(p, r->x + r->w / 2, r->y + r->h / 2), 0);
        KT_EQ(html_hit_link(p, r->x - 40, r->y + r->h / 2), -1);
    }
    html_free(p);
}

KTEST(html, fragment_and_javascript_links_are_ignored){
    const char* src = "<a href=\"#top\">a</a><a href=\"javascript:void(0)\">b</a>"
                      "<a href=\"/real\">c</a>";
    html_page_t* p = html_layout(src, strlen(src), 600, 0);
    KT_NOTNULL(p);
    KT_EQ(p->link_n, 1);
    char href[URL_MAX];
    KT_EQ(html_link_href(p, 0, href, sizeof(href)), 0);
    KT_STREQ(href, "/real");
    html_free(p);
}

KTEST(html, headings_use_the_type_scale){
    const char* src = "<h1>One</h1><h2>Two</h2><h3>Three</h3><p>body</p>";
    html_page_t* p = html_layout(src, strlen(src), 600, 0);
    KT_NOTNULL(p);
    KT_EQ(count_face(p, HTML_FACE_H1), 1);
    KT_EQ(count_face(p, HTML_FACE_H2), 1);
    KT_EQ(count_face(p, HTML_FACE_H3), 1);
    KT_EQ(count_face(p, HTML_FACE_BODY), 1);
    // A hairline under each of the top two levels.
    KT_EQ(count_boxes(p, HTML_BOX_RULE), 2);
    html_free(p);
}

KTEST(html, bold_and_italic_select_faces){
    const char* src = "<p>a <b>bold</b> <i>ital</i> <em>emph</em> <strong>str</strong></p>";
    html_page_t* p = html_layout(src, strlen(src), 600, 0);
    KT_NOTNULL(p);
    KT_EQ(count_face(p, HTML_FACE_BOLD), 2);
    KT_EQ(count_face(p, HTML_FACE_ITALIC), 2);
    KT_EQ(count_face(p, HTML_FACE_BODY), 1);      // just "a"
    html_free(p);
}

KTEST(html, style_stack_unwinds_past_a_stray_close){
    // Real pages leave tags unclosed constantly. One stray </div> in the
    // middle of a bold run must not reset the rest of the document to body.
    const char* src = "<p><b>one</b></div><b>two</b></p>";
    html_page_t* p = html_layout(src, strlen(src), 600, 0);
    KT_NOTNULL(p);
    KT_EQ(count_face(p, HTML_FACE_BOLD), 2);
    html_free(p);
}

KTEST(html, unordered_list_emits_bullets){
    const char* src = "<ul><li>one</li><li>two</li><li>three</li></ul>";
    html_page_t* p = html_layout(src, strlen(src), 600, 0);
    KT_NOTNULL(p);
    KT_EQ(count_boxes(p, HTML_BOX_BULLET), 3);
    html_free(p);
}

KTEST(html, ordered_list_numbers_and_indents){
    const char* src = "<ol><li>one</li><li>two</li></ol>";
    html_page_t* p = html_layout(src, strlen(src), 600, 0);
    KT_NOTNULL(p);
    KT_EQ(count_boxes(p, HTML_BOX_BULLET), 0);    // numbers, not bullets
    char buf[64];
    page_text(p, buf, sizeof(buf));
    KT_STREQ(buf, "1. one 2. two");
    html_free(p);
}

KTEST(html, blockquote_draws_an_edge){
    const char* src = "<blockquote><p>quoted</p></blockquote>";
    html_page_t* p = html_layout(src, strlen(src), 600, 0);
    KT_NOTNULL(p);
    KT_EQ(count_boxes(p, HTML_BOX_BAR), 1);
    html_free(p);
}

KTEST(html, hr_draws_a_rule){
    html_page_t* p = html_layout("<p>a</p><hr><p>b</p>", 19, 600, 0);
    KT_NOTNULL(p);
    KT_EQ(count_boxes(p, HTML_BOX_RULE), 1);
    html_free(p);
}

KTEST(html, pre_preserves_newlines_and_uses_mono){
    const char* src = "<pre>one\ntwo</pre>";
    html_page_t* p = html_layout(src, strlen(src), 600, 0);
    KT_NOTNULL(p);
    KT_EQ(count_face(p, HTML_FACE_MONO), 2);
    KT_EQ(count_boxes(p, HTML_BOX_PANEL), 1);
    // Preserved newline: the two runs are on different lines.
    KT_TRUE(p->run_n >= 2);
    if (p->run_n >= 2) KT_TRUE(p->runs[1].y > p->runs[0].y);
    html_free(p);
}

KTEST(html, images_become_frames_with_alt){
    const char* src = "<img src=\"/a.png\" alt=\"a picture\" width=\"200\" height=\"120\">";
    html_page_t* p = html_layout(src, strlen(src), 600, 0);
    KT_NOTNULL(p);
    KT_EQ(count_boxes(p, HTML_BOX_FRAME), 1);
    char buf[64];
    page_text(p, buf, sizeof(buf));
    KT_STREQ(buf, "a picture");
    html_free(p);
}

KTEST(html, tracking_pixels_are_dropped){
    const char* src = "<p>x</p><img src=\"/t.gif\" width=\"1\" height=\"1\">";
    html_page_t* p = html_layout(src, strlen(src), 600, 0);
    KT_NOTNULL(p);
    KT_EQ(count_boxes(p, HTML_BOX_FRAME), 0);
    html_free(p);
}

KTEST(html, icon_images_contribute_alt_inline_not_a_frame){
    // Wikipedia's protection padlock. A frame here would put an empty box in
    // the middle of a sentence.
    const char* src = "<p>a <img src=\"/lock.png\" alt=\"locked\" width=\"20\" height=\"20\"> b</p>";
    html_page_t* p = html_layout(src, strlen(src), 600, 0);
    KT_NOTNULL(p);
    KT_EQ(count_boxes(p, HTML_BOX_FRAME), 0);
    char buf[64];
    page_text(p, buf, sizeof(buf));
    KT_STREQ(buf, "a locked b");
    html_free(p);
}

KTEST(html, image_without_alt_falls_back_to_the_filename){
    const char* src = "<img src=\"/x/250px-Some_Picture.jpg\" width=\"250\" height=\"200\">";
    html_page_t* p = html_layout(src, strlen(src), 600, 0);
    KT_NOTNULL(p);
    char buf[64];
    page_text(p, buf, sizeof(buf));
    KT_STREQ(buf, "Some Picture");
    html_free(p);
}

KTEST(html, meta_refresh_is_captured){
    const char* src = "<meta http-equiv=\"refresh\" content=\"0; url=https://x.example/y\">";
    html_page_t* p = html_layout(src, strlen(src), 600, 0);
    KT_NOTNULL(p);
    KT_STREQ(p->refresh, "https://x.example/y");
    html_free(p);
}

KTEST(html, text_wraps_and_reflows_with_width){
    // The same source at two widths must produce the same words, a different
    // number of lines, and a taller page when narrower. This is the property
    // the browser's resize path depends on.
    const char* src = "<p>alpha bravo charlie delta echo foxtrot golf hotel india "
                      "juliet kilo lima mike november oscar papa quebec romeo</p>";
    html_page_t* wide = html_layout(src, strlen(src), 900, 0);
    html_page_t* narrow = html_layout(src, strlen(src), 300, 0);
    KT_NOTNULL(wide);
    KT_NOTNULL(narrow);

    char a[256], b[256];
    page_text(wide, a, sizeof(a));
    page_text(narrow, b, sizeof(b));
    KT_STREQ(a, b);                          // same words
    KT_TRUE(narrow->height > wide->height);  // more lines

    // Nothing may be laid out past the right edge of its measure.
    for (uint32_t i = 0; i < narrow->run_n; i++)
        KT_TRUE(narrow->runs[i].x < 300);

    html_free(wide);
    html_free(narrow);
}

KTEST(html, wide_viewport_caps_the_measure){
    // A very wide window must produce margins, not 200-character lines.
    const char* src = "<p>alpha bravo charlie delta echo foxtrot golf hotel india "
                      "juliet kilo lima mike november oscar papa quebec romeo sierra</p>";
    html_page_t* p = html_layout(src, strlen(src), 2000, 0);
    KT_NOTNULL(p);
    int32_t max_x = 0, min_x = 0x7FFFFFFF;
    for (uint32_t i = 0; i < p->run_n; i++){
        if (p->runs[i].x < min_x) min_x = p->runs[i].x;
        int32_t right = p->runs[i].x + p->runs[i].w;
        if (right > max_x) max_x = right;
    }
    // The measure is capped and centred, so the text sits in a column in the
    // middle of the window rather than running the full 2000px.
    KT_TRUE(max_x - min_x <= 700);
    KT_TRUE(min_x > 300);
    html_free(p);
}

KTEST(html, plain_text_mode_keeps_line_structure){
    const char* src = "line one\nline two\nline three";
    html_page_t* p = html_layout(src, strlen(src), 600, 1);
    KT_NOTNULL(p);
    KT_EQ(count_face(p, HTML_FACE_MONO), 6);   // two words per line, three lines
    // Tags are not interpreted in plain mode.
    const char* tagged = "a <b>not bold</b>";
    html_page_t* q = html_layout(tagged, strlen(tagged), 600, 1);
    KT_NOTNULL(q);
    KT_EQ(count_face(q, HTML_FACE_BOLD), 0);
    html_free(p);
    html_free(q);
}

KTEST(html, empty_and_degenerate_input){
    html_page_t* a = html_layout("", 0, 600, 0);
    KT_NOTNULL(a);
    KT_EQ(a->run_n, 0);
    html_free(a);

    // Unterminated tag, unterminated comment, lone angle brackets: none of
    // these may run off the end of the buffer.
    html_page_t* b = html_layout("<p>x<div class=", 15, 600, 0);
    KT_NOTNULL(b);
    html_free(b);

    html_page_t* c = html_layout("<!-- never closed", 17, 600, 0);
    KT_NOTNULL(c);
    html_free(c);

    html_page_t* d = html_layout("a < b > c", 9, 600, 0);
    KT_NOTNULL(d);
    html_free(d);
}

KTEST(html, deeply_nested_markup_does_not_run_away){
    // The style stack is bounded; exceeding it must degrade, not corrupt.
    static char src[4096];
    uint32_t o = 0;
    for (int i = 0; i < 100 && o < sizeof(src) - 16; i++) o += (uint32_t)ksnprintf(src + o, sizeof(src) - o, "<b>");
    o += (uint32_t)ksnprintf(src + o, sizeof(src) - o, "deep");
    for (int i = 0; i < 100 && o < sizeof(src) - 16; i++) o += (uint32_t)ksnprintf(src + o, sizeof(src) - o, "</b>");

    html_page_t* p = html_layout(src, o, 600, 0);
    KT_NOTNULL(p);
    char buf[64];
    page_text(p, buf, sizeof(buf));
    KT_STREQ(buf, "deep");
    html_free(p);
}
