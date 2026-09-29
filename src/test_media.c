// src/test_media.c — the pieces YouTube playback stands on
//
// URL resolution against a server on a non-default port (the YouTube
// gateway), heap growth for stream and image buffers, FPU state surviving
// preemption (the MP2 decoder is float from end to end), image decoding and
// its cache, and the markup side of <img> and <video>. The decoders and the
// sound card themselves are exercised by booting the desktop against the
// gateway -- see tools/ytgate.py -- because they need a network stream and a
// device the headless test machine does not have.
#include "header/ktest.h"
#include "header/kstring.h"
#include "header/kheap.h"
#include "header/task.h"
#include "header/http.h"
#include "header/html.h"
#include "header/image.h"

extern volatile unsigned long long ticks;

// ------------------------------------------------------------------ URLs

KTEST(url, resolve_root_relative_keeps_the_port){
    url_t base;
    char out[URL_MAX];
    KT_EQ(url_parse("http://10.0.2.2:8090/results?search_query=cats", &base), 0);
    url_resolve(&base, "/watch?v=abc", out, sizeof(out));
    KT_STREQ(out, "http://10.0.2.2:8090/watch?v=abc");
}

KTEST(url, resolve_bare_query_replaces_only_the_query){
    url_t base;
    char out[URL_MAX];
    KT_EQ(url_parse("https://example.com/results?page=1", &base), 0);
    url_resolve(&base, "?page=2", out, sizeof(out));
    KT_STREQ(out, "https://example.com/results?page=2");
}

KTEST(url, resolve_relative_ignores_slashes_in_the_query){
    url_t base;
    char out[URL_MAX];
    KT_EQ(url_parse("http://h.example/a/page?next=/x/y", &base), 0);
    url_resolve(&base, "img.png", out, sizeof(out));
    KT_STREQ(out, "http://h.example/a/img.png");
}

KTEST(url, resolve_default_port_is_not_spelled_out){
    url_t base;
    char out[URL_MAX];
    KT_EQ(url_parse("https://example.com:443/a", &base), 0);
    url_resolve(&base, "/b", out, sizeof(out));
    KT_STREQ(out, "https://example.com/b");
}

// ------------------------------------------------------------------ heap

KTEST(kheap, krealloc_keeps_contents_while_growing){
    uint8_t* p = (uint8_t*)kmalloc(100);
    KT_TRUE(p != 0);
    if (!p) return;
    for (int i = 0; i < 100; i++) p[i] = (uint8_t)(i * 7);

    uint8_t* q = (uint8_t*)krealloc(p, 300000);
    KT_TRUE(q != 0);
    if (!q){ kfree(p); return; }
    int same = 1;
    for (int i = 0; i < 100; i++) if (q[i] != (uint8_t)(i * 7)) same = 0;
    KT_TRUE(same);
    q[299999] = 0x5A;                  // the whole new extent is writable
    KT_EQ(q[299999], 0x5A);
    kfree(q);
}

KTEST(kheap, krealloc_of_null_allocates_and_to_zero_frees){
    void* p = krealloc(0, 64);
    KT_TRUE(p != 0);
    KT_TRUE(krealloc(p, 0) == 0);
}

KTEST(kheap, a_run_of_free_blocks_merges_into_one){
    // Three neighbours freed in an order that used to leave a seam: the
    // combined size has to be allocatable again at the first one's address.
    uint8_t* a = (uint8_t*)kmalloc(4096);
    uint8_t* b = (uint8_t*)kmalloc(4096);
    uint8_t* c = (uint8_t*)kmalloc(4096);
    KT_TRUE(a && b && c);
    if (!a || !b || !c) return;
    KT_TRUE(b > a && c > b);
    kfree(b);
    kfree(c);
    kfree(a);
    uint8_t* big = (uint8_t*)kmalloc(3 * 4096);
    KT_TRUE(big == a);
    kfree(big);
}

KTEST(kheap, has_room_for_a_video_ring_and_its_frames){
    // What video_open asks for, all at once, while nothing else is playing.
    void* ring = kmalloc(8u * 1024u * 1024u);
    void* f0   = kmalloc(320u * 180u * 4u);
    void* f1   = kmalloc(320u * 180u * 4u);
    KT_TRUE(ring && f0 && f1);
    if (ring) kfree(ring);
    if (f0) kfree(f0);
    if (f1) kfree(f1);
}

// ------------------------------------------------------------------- FPU

// Two tasks accumulate in floating point long enough to be preempted many
// times each. If the scheduler did not carry the x87 state across switches,
// each would pick up the other's stack registers and the sums would be wrong.
static volatile double fpu_sum[2];
static volatile int    fpu_done[2];

static void fpu_worker(void* arg){
    int which = (int)(uintptr_t)arg;
    double k = which ? 0.25 : 0.5;
    double acc = 0.0;
    for (uint32_t i = 0; i < 2000000u; i++) acc += (double)i * k;
    fpu_sum[which]  = acc;
    fpu_done[which] = 1;
}

KTEST(fpu, state_survives_preemption){
    fpu_done[0] = fpu_done[1] = 0;
    fpu_sum[0]  = fpu_sum[1]  = 0.0;
    KT_TRUE(task_create("fpu-a", fpu_worker, (void*)0) >= 0);
    KT_TRUE(task_create("fpu-b", fpu_worker, (void*)1) >= 0);

    unsigned long long deadline = ticks + 3000;
    double mine = 0.0;
    while ((!fpu_done[0] || !fpu_done[1]) && ticks < deadline){
        for (int i = 0; i < 1000; i++) mine += 1.5;    // this task uses it too
        task_sleep(10);
    }
    KT_TRUE(fpu_done[0] && fpu_done[1]);

    // Sums of i*k over i < N are exact in a double at this size.
    double n = 2000000.0;
    double base = n * (n - 1.0) / 2.0;
    KT_TRUE(fpu_sum[0] == base * 0.5);
    KT_TRUE(fpu_sum[1] == base * 0.25);
    KT_TRUE(mine > 0.0);
}

// ---------------------------------------------------------------- images

// 3x2 RGBA PNG: red, green, blue / white, black, fully transparent red.
static const uint8_t tiny_png[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x9D, 0x74, 0x66,
    0x1A, 0x00, 0x00, 0x00, 0x18, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0xF8, 0xCF, 0xC0, 0xF0,
    0x1F, 0x0C, 0x19, 0xFE, 0x03, 0x49, 0x20, 0x60, 0x00, 0xB3, 0x18, 0x18, 0x00, 0xA4, 0x75, 0x0B,
    0xF5, 0x4C, 0xB1, 0x0D, 0x3A, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60,
    0x82
};

KTEST(image, decodes_png_and_composites_transparency){
    image_t* img = image_decode(tiny_png, sizeof(tiny_png), 0, 0x00123456);
    KT_TRUE(img != 0);
    if (!img) return;
    KT_EQ(img->w, 3);
    KT_EQ(img->h, 2);
    KT_EQ(img->px[0], 0x00FF0000);
    KT_EQ(img->px[1], 0x0000FF00);
    KT_EQ(img->px[2], 0x000000FF);
    KT_EQ(img->px[3], 0x00FFFFFF);
    KT_EQ(img->px[4], 0x00000000);
    KT_EQ(img->px[5], 0x00123456);     // alpha 0: the background shows through
    image_free(img);
}

KTEST(image, max_width_scales_down_keeping_shape){
    image_t* img = image_decode(tiny_png, sizeof(tiny_png), 2, 0);
    KT_TRUE(img != 0);
    if (!img) return;
    KT_EQ(img->w, 2);
    KT_EQ(img->h, 1);
    image_free(img);
}

// 2x1 GIF: a red pixel then a blue one.
static const uint8_t tiny_gif[] = {
    0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x02, 0x00, 0x01, 0x00, 0x80, 0x00, 0x00, 0xFF, 0x00, 0x00,
    0x00, 0x00, 0xFF, 0x2C, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x02, 0x02, 0x44,
    0x0A, 0x00, 0x3B
};

static volatile int gif_done;
static volatile uint32_t gif_px[2];
static volatile int gif_ok;

static void gif_worker(void* arg){
    (void)arg;
    image_t* img = image_decode(tiny_gif, sizeof(tiny_gif), 0, 0);
    gif_ok = img && img->w == 2 && img->h == 1;
    if (gif_ok){ gif_px[0] = img->px[0]; gif_px[1] = img->px[1]; }
    image_free(img);
    gif_done = 1;
}

// The browser decodes images on an ordinary kernel task. stb_image's GIF
// decoder keeps 34 KB of LZW state on the stack, which overran the 32 KB task
// stacks and zeroed the heap header below -- the heap then looked a few
// hundred KB long and every later connection failed for lack of memory.
KTEST(image, gif_decodes_on_a_task_stack_without_damaging_the_heap){
    gif_done = 0;
    gif_ok = 0;
    KT_EQ(kheap_check(), 0);
    KT_TRUE(task_create("gif", gif_worker, 0) >= 0);
    unsigned long long deadline = ticks + 500;
    while (!gif_done && ticks < deadline) task_sleep(10);
    KT_TRUE(gif_done);
    KT_TRUE(gif_ok);
    KT_EQ(gif_px[0], 0x00FF0000);
    KT_EQ(gif_px[1], 0x000000FF);
    KT_EQ(kheap_check(), 0);
}

KTEST(image, garbage_is_rejected_not_fatal){
    static const uint8_t junk[] = "this is not an image at all, just text";
    KT_TRUE(image_decode(junk, sizeof(junk), 0, 0) == 0);
    KT_TRUE(image_decode(tiny_png, 20, 0, 0) == 0);   // truncated
}

KTEST(image, cache_claims_once_and_serves_the_result){
    const char* url = "http://test.invalid/ktest-image.png";
    KT_EQ(image_cache_get(url, 0), IMG_NONE);
    KT_EQ(image_cache_claim(url), 1);
    KT_EQ(image_cache_claim(url), 0);                  // already pending
    KT_EQ(image_cache_get(url, 0), IMG_PENDING);

    image_cache_put(url, image_decode(tiny_png, sizeof(tiny_png), 0, 0));
    const image_t* got = 0;
    KT_EQ(image_cache_get(url, &got), IMG_READY);
    KT_TRUE(got != 0 && got->w == 3);

    const char* bad = "http://test.invalid/ktest-missing.png";
    KT_EQ(image_cache_claim(bad), 1);
    image_cache_put(bad, 0);
    KT_EQ(image_cache_get(bad, 0), IMG_FAILED);
    KT_EQ(image_cache_claim(bad), 0);                  // a failure is not retried
}

// ------------------------------------------------------------------ markup

static const html_box_t* find_box(const html_page_t* p, int kind, uint32_t* idx){
    for (uint32_t i = 0; i < p->box_n; i++)
        if (p->boxes[i].kind == kind){ if (idx) *idx = i; return &p->boxes[i]; }
    return 0;
}

KTEST(html, video_tag_becomes_a_video_box_with_its_source){
    static const char src[] =
        "<h2>Title</h2><video src=\"/stream/abc.mpg\" width=\"640\" height=\"360\"></video><p>after</p>";
    html_page_t* p = html_layout(src, sizeof(src) - 1, 700, 0);
    KT_TRUE(p != 0);
    if (!p) return;
    uint32_t i = 0;
    const html_box_t* b = find_box(p, HTML_BOX_VIDEO, &i);
    KT_TRUE(b != 0);
    if (b){
        KT_EQ(b->w, 640);
        KT_EQ(b->h, 360);
        char out[URL_MAX];
        KT_EQ(html_box_src(p, i, out, sizeof(out)), 0);
        KT_STREQ(out, "/stream/abc.mpg");
    }
    html_free(p);
}

KTEST(html, source_child_names_the_video_stream){
    static const char src[] = "<video width=\"320\" height=\"180\"><source src=\"clip.mpg\"></video>";
    html_page_t* p = html_layout(src, sizeof(src) - 1, 700, 0);
    KT_TRUE(p != 0);
    if (!p) return;
    uint32_t i = 0;
    KT_TRUE(find_box(p, HTML_BOX_VIDEO, &i) != 0);
    char out[URL_MAX];
    KT_EQ(html_box_src(p, i, out, sizeof(out)), 0);
    KT_STREQ(out, "clip.mpg");
    html_free(p);
}

KTEST(html, image_inside_a_link_is_clickable){
    static const char src[] =
        "<a href=\"/watch?v=xyz\"><img src=\"https://i.ytimg.com/vi/xyz/mqdefault.jpg\" "
        "width=\"320\" height=\"180\" alt=\"A video\"></a>";
    html_page_t* p = html_layout(src, sizeof(src) - 1, 700, 0);
    KT_TRUE(p != 0);
    if (!p) return;
    uint32_t i = 0;
    const html_box_t* b = find_box(p, HTML_BOX_FRAME, &i);
    KT_TRUE(b != 0);
    if (b){
        char out[URL_MAX];
        KT_EQ(html_box_src(p, i, out, sizeof(out)), 0);
        KT_STREQ(out, "https://i.ytimg.com/vi/xyz/mqdefault.jpg");

        // A corner of the thumbnail, well away from the alt text in its middle.
        int link = html_hit_link(p, b->x + 4, b->y + 4);
        KT_EQ(link, 0);
        KT_EQ(html_link_href(p, link, out, sizeof(out)), 0);
        KT_STREQ(out, "/watch?v=xyz");
    }
    html_free(p);
}

KTEST(html, form_fields_are_recorded_with_their_form){
    static const char src[] =
        "<form action=\"/results\"><input type=\"search\" name=\"search_query\" placeholder=\"Search\">"
        "<input type=\"hidden\" name=\"hl\" value=\"en\"><input type=\"submit\" value=\"Go\"></form>"
        "<input type=\"text\" name=\"outside\">";
    html_page_t* p = html_layout(src, sizeof(src) - 1, 700, 0);
    KT_TRUE(p != 0);
    if (!p) return;
    KT_EQ(p->form_n, 1);
    KT_EQ(p->field_n, 4);
    if (p->form_n == 1 && p->field_n == 4){
        char buf[64];
        html_slice(p, p->forms[0].action_off, p->forms[0].action_len, buf, sizeof(buf));
        KT_STREQ(buf, "/results");

        KT_EQ(p->fields[0].kind, HTML_FIELD_TEXT);
        KT_EQ(p->fields[0].form, 0);
        html_slice(p, p->fields[0].name_off, p->fields[0].name_len, buf, sizeof(buf));
        KT_STREQ(buf, "search_query");
        html_slice(p, p->fields[0].value_off, p->fields[0].value_len, buf, sizeof(buf));
        KT_STREQ(buf, "");                     // a placeholder is not a value

        KT_EQ(p->fields[1].kind, HTML_FIELD_HIDDEN);
        KT_EQ(p->fields[1].box, -1);
        html_slice(p, p->fields[1].value_off, p->fields[1].value_len, buf, sizeof(buf));
        KT_STREQ(buf, "en");

        KT_EQ(p->fields[2].kind, HTML_FIELD_SUBMIT);
        KT_EQ(p->fields[3].form, -1);          // after </form>

        const html_box_t* b = &p->boxes[p->fields[0].box];
        KT_EQ(html_field_at(p, b->x + 5, b->y + 5), 0);
    }
    html_free(p);
}

KTEST(html, lazy_image_prefers_data_src_over_a_data_uri){
    static const char src[] =
        "<img src=\"data:image/gif;base64,R0lGOD\" data-src=\"/real.jpg\" width=\"200\" height=\"100\">";
    html_page_t* p = html_layout(src, sizeof(src) - 1, 700, 0);
    KT_TRUE(p != 0);
    if (!p) return;
    uint32_t i = 0;
    KT_TRUE(find_box(p, HTML_BOX_FRAME, &i) != 0);
    char out[URL_MAX];
    KT_EQ(html_box_src(p, i, out, sizeof(out)), 0);
    KT_STREQ(out, "/real.jpg");
    html_free(p);
}
