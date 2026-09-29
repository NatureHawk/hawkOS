// src/app_browser.c — the Browser
//
// The fetch runs on its own task and the window only ever reads a snapshot of
// its result. That is what keeps the desktop alive while a page is loading:
// a TLS handshake plus a Wikipedia download takes several seconds of solid
// work on an emulated 486-class CPU, and doing it on the compositor's task
// would freeze every window on screen.
//
// Tabs each own their own page, scroll position and history. Only one fetch
// is in flight at a time — the network task, the TCP connection table and the
// 128 MB budget all favour finishing one page before starting the next — so
// a tab requests a load and the single fetch task serves whichever tab asked.
#include <stdint.h>
#include "header/apps.h"
#include "header/wm.h"
#include "header/theme.h"
#include "header/gfx.h"
#include "header/kheap.h"
#include "header/kstring.h"
#include "header/kbd.h"
#include "header/clipboard.h"
#include "header/task.h"
#include "header/http.h"
#include "header/html.h"
#include "header/net.h"
#include "header/netcfg.h"
#include "header/font.h"
#include "header/image.h"
#include "header/video.h"
#include "header/irqctl.h"
#include "header/ac97.h"

extern volatile unsigned long long ticks;

#define TABBAR_H    30
#define TOOLBAR_H   38
#define STATUS_H    24
#define SCROLLBAR_W 12
#define HISTORY_MAX 24
#define MAX_TABS    6
#define TAB_W       180
#define TAB_CLOSE_W 16
#define NEWTAB_W    30

enum { BR_IDLE = 0, BR_LOADING, BR_READY, BR_ERROR };

// Search goes through DuckDuckGo's "lite" endpoint. It is the one major
// search engine that still returns a complete result page as plain server-
// rendered HTML: Google's results are assembled by JavaScript, which this
// browser has no way to run.
#define SEARCH_PREFIX "https://lite.duckduckgo.com/lite/?q="
#define YT_SEARCH     "https://www.youtube.com/results?search_query="

// Images waiting for the fetch task. Filled by the window from the page on
// screen, drained between page loads; a page load always goes first.
#define IMG_Q         48

// Widest an image is stored at. Nothing is laid out wider than the measure
// (html.c's MAX_LINE_W), so anything larger is memory spent on pixels that
// are scaled away on every paint.
#define IMG_MAX_W     720

// Video qualities offered on the player's control bar, as the gateway names
// them (tools/ytgate.py, QUALITIES). The default is the largest the emulated
// CPU decodes at full frame rate; the others are there for a faster machine,
// or a slower one.
static const int QUALITY[] = { 144, 240, 360, 480, 720 };
#define QUALITY_N       5
#define QUALITY_DEFAULT 480

#define CTL_H           34      // the control bar along the bottom of a video
#define SEEK_STEP_MS    10000   // Left / Right
#define SEEK_SETTLE     30      // ticks without another seek before one is carried out

// Pages already downloaded, kept so Back, and any return to a page seen in
// the last few minutes, is a re-layout rather than another DNS lookup, TLS
// handshake and download. Reload always goes to the network.
#define PAGE_CACHE_N      24
#define PAGE_CACHE_BUDGET (16u * 1024u * 1024u)
#define PAGE_CACHE_TTL    (10u * 60u * 100u)      // ticks: ten minutes

typedef struct {
    int             used;
    char            url[URL_MAX];
    int             url_len;
    char            title[44];

    volatile int    state;
    char            status[128];

    http_response_t resp;
    html_page_t*    page;
    int             scroll;

    // What the current layout was built from, so a resize can rebuild it at
    // the new width. This is kept rather than inferred from `state` because
    // state is a transient signal -- BR_READY means "a response just landed",
    // and on_tick clears it to BR_IDLE the moment it has been laid out, so by
    // the time a window is resized there is nothing left in it to test.
    //
    // It points either at a generated page in .rodata or at this tab's
    // resp.body, both of which outlive the layout. navigate() clears it for
    // the duration of a fetch, so it can never be followed into a buffer the
    // fetch task is replacing.
    const char*     src;
    uint32_t        src_len;
    int             src_plain;
    int             src_keep_chrome;   // reader mode was turned off for this one

    char            history[HISTORY_MAX][URL_MAX];
    int             history_scroll[HISTORY_MAX];   // where the reader was on each
    int             history_n;

    // Scroll position to restore once the page now loading is laid out, or
    // -1. Set by Back, so returning to a long article lands where you left.
    int             pending_scroll;

    // Bounded so a page that refreshes to itself cannot spin forever.
    int             refresh_hops;

    // The <video> on this page, once it has been started, and which box of
    // the display list it plays into. video_tried stops a stream that failed
    // to open from being retried on every tick.
    video_t*        video;
    int             video_box;
    int             video_tried;

    // The stream being replaced by a seek or a quality change. It carries on
    // playing, picture and sound, until the new one has buffered and taken
    // over the sound card -- the way YouTube switches quality -- so a switch
    // costs no silence however long the new stream takes to start.
    video_t*        video_prev;
    // The stream's address without its query, to which ?q= and &t= are added.
    char            video_src[URL_MAX];

    // A seek waiting to happen. Every seek restarts the stream, so a burst of
    // them (Right pressed three times) is gathered into one: each press moves
    // the target, and the stream restarts once they stop coming.
    int64_t         seek_target;        // ms, or -1
    unsigned long long seek_at;

    // Auto quality: when the stream was started, and how many seconds in a
    // row it has played too slowly.
    unsigned long long video_started, slow_mark;
    int             slow_secs;

    // Form fields typed into on this page: which field, and its text. The
    // field with the keyboard is field_focus (-1 for none).
    struct { int field; char text[160]; } edits[8];
    int             edit_n;
    int             field_focus;

    // The "gateway not running" page has been shown for the current error.
    int             gateway_notice;
} tab_t;

typedef struct {
    tab_t        tabs[MAX_TABS];
    int          cur;

    int          url_focus;
    // Clicking the address bar selects its contents, the way every other
    // browser behaves: the next character typed replaces the old URL instead
    // of being appended to it.
    int          url_selected;

    int          view_w, view_h;
    int          drag_scroll;

    // Text selected on the page. The anchor is where the press landed and the
    // head follows the pointer; either can come first in the document. The
    // page the positions refer to is remembered so a selection is dropped the
    // moment that page is laid out again, when its run numbers stop meaning
    // anything.
    html_pos_t   sel_anchor, sel_head;
    const html_page_t* sel_page;
    int          sel_drag, sel_moved, press_x, press_y, press_link;

    // Fetch handoff. The window writes the request and raises fetch_go; the
    // fetch task lowers it and works on fetch_tab. One request at a time, so
    // no queue is needed.
    volatile int fetch_go;
    volatile int fetch_tab;
    char         fetch_url[URL_MAX];

    // Generated markup for a page the browser has to explain rather than
    // show. It lives here rather than on the stack because a laid-out page
    // keeps a pointer to the source it was built from, for reflow on resize.
    char         notice[1600];

    // Image fetch queue (see IMG_Q), and the response the fetch task decodes
    // images out of -- kept here rather than on its stack, which a TLS
    // handshake already uses most of.
    char            img_q[IMG_Q][URL_MAX];
    int             img_head, img_tail;
    http_response_t img_resp;

    // Page cache (PAGE_CACHE_*). Only the fetch task reads or writes it, so
    // it needs no lock. fetch_fresh is raised by Reload to bypass it.
    struct {
        char               url[URL_MAX];
        http_response_t    resp;
        unsigned long long stored_at;
        uint32_t           used_at;
    }               cache[PAGE_CACHE_N];
    uint32_t        cache_clock;
    uint32_t        cache_bytes;
    volatile int    fetch_fresh;

    // Player settings, shared by every tab.
    int             quality;
    int             quality_manual;     // the user picked it: auto quality stays out of it
    int             volume;             // 0-100
    int             muted;
    int             fullscreen;         // the current tab's video fills the view
    int             fs_maximized;       // fullscreen maximised the window itself
    wm_window_t*    win;
} browser_t;

// Below this many characters, a document has not really rendered. Picked to
// sit above a stray "Loading..." or copyright line and well below the
// shortest real page: example.com sets 109.
#define EMPTY_CHARS 48

// One browser window at a time: the fetch task needs a stable pointer to the
// state it is filling in, and a second concurrent fetch would need a second
// TCP connection budget besides.
static browser_t* active = 0;

static const char* HOME_PAGE =
    "<h1>hawkOS Browser</h1>"
    "<p>Written from scratch: our own TCP/IP stack over an RTL8139 driver, "
    "DHCP and DNS, HTTP on top, TLS via BearSSL, and this renderer.</p>"
    "<h2>Try these</h2>"
    "<ul>"
    "<li><a href=\"https://www.youtube.com/\">YouTube</a> - search and watch videos</li>"
    "<li><a href=\"https://en.wikipedia.org/wiki/Operating_system\">Wikipedia: Operating system</a></li>"
    "<li><a href=\"https://en.wikipedia.org/wiki/MS-DOS\">Wikipedia: MS-DOS</a></li>"
    "<li><a href=\"https://lite.duckduckgo.com/lite/?q=hobby+operating+system\">A search results page</a></li>"
    "<li><a href=\"https://example.com\">example.com over HTTPS</a></li>"
    "<li><a href=\"http://example.com\">example.com over plain HTTP</a></li>"
    "</ul>"
    "<h2>Using it</h2>"
    "<ul>"
    "<li>Type a URL in the address bar, or any words to search</li>"
    "<li>Start with <b>yt</b> to search YouTube: yt lofi hip hop</li>"
    "<li>Videos: Space pauses, Left / Right skip 10 s, F is fullscreen, "
    "M mutes, + and - set the volume, 1-5 pick 144p to 720p</li>"
    "<li>Ctrl+T opens a tab, Ctrl+W closes one, Ctrl+R reloads</li>"
    "<li>Up / Down / PageUp / PageDown scroll, Home jumps to the top</li>"
    "<li>Backspace goes back</li>"
    "</ul>"
    "<p>Certificates are not verified against a trust store - see the note at "
    "the top of src/tls.c. Traffic is encrypted, but this is not a browser to "
    "type a password into.</p>";

static tab_t* cur_tab(browser_t* b){ return &b->tabs[b->cur]; }

// ---------------------------------------------------------------- YouTube
//
// youtube.com is two things no browser of this size can do. The page that
// arrives is a script that builds the page, so without a JavaScript engine it
// is blank; and the video is VP9, AV1 or H.264 in fragmented MP4, handed out
// through signed URLs that only YouTube's own player code can compute.
//
// So YouTube is reached through a gateway: tools/ytgate.py, running on the
// host machine. It talks to YouTube with yt-dlp and answers in what hawkOS
// can use -- server-rendered HTML for the pages, JPEG thumbnails, and each
// video transcoded on the fly to an MPEG-1 program stream, which the kernel
// decodes itself (src/video.c). Every request for a YouTube host goes there;
// the address bar and history keep the youtube.com address, because that is
// what the user asked for and what a link copied out of the bar should be.
//
// Under QEMU's user-mode network the host is always 10.0.2.2.
#define YT_GATEWAY      "http://10.0.2.2:8090"
#define YT_GATEWAY_LEN  (sizeof(YT_GATEWAY) - 1)

static int is_youtube_host(const char* h){
    static const char* const H[] = {
        "youtube.com", "www.youtube.com", "m.youtube.com", "music.youtube.com",
        "youtu.be", "www.youtu.be", "i.ytimg.com", "i1.ytimg.com", "i9.ytimg.com"
    };
    for (int i = 0; i < (int)(sizeof(H) / sizeof(H[0])); i++)
        if (kstricmp(h, H[i]) == 0) return 1;
    return 0;
}

static int is_gateway(const char* url){
    return strncmp(url, YT_GATEWAY, YT_GATEWAY_LEN) == 0
        && (url[YT_GATEWAY_LEN] == '/' || url[YT_GATEWAY_LEN] == 0);
}

// A YouTube address to the gateway address that serves it. Returns 1 if
// `url` was a YouTube address.
static int yt_to_gateway(const char* url, char* out, uint32_t cap){
    url_t u;
    if (url_parse(url, &u) != 0 || !is_youtube_host(u.host)) return 0;

    if (kstrnicmp(u.host, "youtu.be", 8) == 0 || kstrnicmp(u.host, "www.youtu.be", 12) == 0){
        char id[64];
        uint32_t n = 0;
        const char* p = u.path + 1;
        while (p[n] && p[n] != '?' && p[n] != '/' && n < sizeof(id) - 1){ id[n] = p[n]; n++; }
        id[n] = 0;
        ksnprintf(out, cap, "%s/watch?v=%s", YT_GATEWAY, id);
    } else if (strstr(u.host, "ytimg.com")){
        ksnprintf(out, cap, "%s/ytimg%s", YT_GATEWAY, u.path);
    } else {
        ksnprintf(out, cap, "%s%s", YT_GATEWAY, u.path);
    }
    return 1;
}

// The other direction, for anything shown to the user.
static void gateway_to_yt(const char* url, char* out, uint32_t cap){
    if (!is_gateway(url)){
        strncpy(out, url, cap - 1);
        out[cap - 1] = 0;
        return;
    }
    const char* path = url + YT_GATEWAY_LEN;
    if (!*path) path = "/";
    if (strncmp(path, "/ytimg/", 7) == 0) ksnprintf(out, cap, "https://i.ytimg.com%s", path + 6);
    else                                  ksnprintf(out, cap, "https://www.youtube.com%s", path);
}

// Where a request for `url` actually goes.
static void fetch_address(const char* url, char* out, uint32_t cap){
    if (!yt_to_gateway(url, out, cap)){
        strncpy(out, url, cap - 1);
        out[cap - 1] = 0;
    }
}

// ------------------------------------------------------------ media helpers

// A box's resource as an absolute URL, resolved against the address the page
// was actually served from. Returns 0 on success.
static int box_abs_src(tab_t* t, uint32_t i, char* out, uint32_t cap){
    char src[URL_MAX];
    if (!t->page || html_box_src(t->page, i, src, sizeof(src)) != 0) return -1;

    url_t base;
    if (t->resp.final_url[0] && url_parse(t->resp.final_url, &base) == 0){
        url_resolve(&base, src, out, cap);
        return out[0] ? 0 : -1;
    }
    if (kstrnicmp(src, "http://", 7) == 0 || kstrnicmp(src, "https://", 8) == 0){
        strncpy(out, src, cap - 1);
        out[cap - 1] = 0;
        return 0;
    }
    if (src[0] == '/' && src[1] == '/'){ ksnprintf(out, cap, "https:%s", src); return 0; }
    return -1;
}

static int img_push(browser_t* b, const char* url){
    uint32_t f = irq_save();
    int ok = 0;
    if (b->img_head - b->img_tail < IMG_Q){
        strncpy(b->img_q[b->img_head % IMG_Q], url, URL_MAX - 1);
        b->img_q[b->img_head % IMG_Q][URL_MAX - 1] = 0;
        b->img_head++;
        ok = 1;
    }
    irq_restore(f);
    return ok;
}

static int img_pop(browser_t* b, char* out){
    uint32_t f = irq_save();
    int ok = 0;
    if (b->img_head != b->img_tail){
        strcpy(out, b->img_q[b->img_tail % IMG_Q]);
        b->img_tail++;
        ok = 1;
    }
    irq_restore(f);
    return ok;
}

static void fetch_image(browser_t* b, const char* url){
    char real[URL_MAX];
    fetch_address(url, real, sizeof(real));

    http_response_t* r = &b->img_resp;
    memset(r, 0, sizeof(*r));
    image_t* img = 0;
    if (http_get(real, r, 0, 0) == 0 && r->status == 200 && r->body_len)
        img = image_decode(r->body, r->body_len, IMG_MAX_W, TH_PAGE_BG);
    http_response_free(r);

    image_cache_put(url, img);
    wm_invalidate();
}

static void stop_video(tab_t* t){
    if (t->video){ video_close(t->video); t->video = 0; }
    if (t->video_prev){ video_close(t->video_prev); t->video_prev = 0; }
    t->video_box   = -1;
    t->video_tried = 0;
    t->seek_target = -1;
}

static void clock_text(char* out, uint32_t cap, uint32_t ms);
static void forms_reset(tab_t* t);

static void apply_volume(browser_t* b){
    ac97_set_volume(b->muted ? 0 : b->volume);
}

// (Re)starts the current tab's video at `at_ms`, at the browser's quality.
static void video_start(browser_t* b, tab_t* t, uint32_t at_ms){
    char url[URL_MAX];
    ksnprintf(url, sizeof(url), "%s?q=%d&t=%u.%03u", t->video_src, b->quality,
              at_ms / 1000u, at_ms % 1000u);
    if (t->video){
        if (t->video_prev) video_close(t->video_prev);
        t->video_prev = t->video;
    }
    t->video = video_open(url, at_ms);
    t->seek_target = -1;
    t->video_started = ticks;
    t->slow_secs = 0;
    apply_volume(b);
}

// Where the viewer is, counting a seek that has not been carried out yet.
static uint32_t video_where(tab_t* t){
    if (t->seek_target >= 0) return (uint32_t)t->seek_target;
    return t->video ? video_position_ms(t->video) : 0;
}

static void video_seek(browser_t* b, tab_t* t, int64_t to_ms){
    (void)b;
    if (!t->video) return;
    uint32_t dur = video_duration_ms(t->video);
    if (dur && to_ms > (int64_t)dur - 1000) to_ms = (int64_t)dur - 1000;
    if (to_ms < 0) to_ms = 0;
    t->seek_target = to_ms;
    t->seek_at     = ticks;
}

static void set_quality(browser_t* b, tab_t* t, int q){
    b->quality_manual = 1;
    if (q == b->quality) return;
    b->quality = q;
    if (t->video) video_start(b, t, video_where(t));
}

// Fullscreen is the video filling the browser's page area, with the window
// maximised to make that area as large as the screen allows. Leaving puts
// the window back only if entering is what maximised it.
static void set_fullscreen(browser_t* b, int on){
    if (on == b->fullscreen) return;
    b->fullscreen = on;
    if (!b->win) return;
    if (on && !b->win->maximized){ wm_maximize(b->win, 1); b->fs_maximized = 1; }
    if (!on && b->fs_maximized){ wm_maximize(b->win, 0); b->fs_maximized = 0; }
}

static void toggle_play(browser_t* b, tab_t* t){
    if (!t->video) return;
    if (video_state(t->video) == VIDEO_ENDED) video_start(b, t, 0);
    else                                      video_toggle_pause(t->video);
}

// ------------------------------------------------------------ page loading

static void progress(void* ctx, const char* stage){
    browser_t* b = (browser_t*)ctx;
    int t = b->fetch_tab;
    if (t < 0 || t >= MAX_TABS) return;
    strncpy(b->tabs[t].status, stage, sizeof(b->tabs[t].status) - 1);
    b->tabs[t].status[sizeof(b->tabs[t].status) - 1] = 0;
    wm_invalidate();
}

static int cacheable(const http_response_t* r){
    if (r->status != 200 || !r->body || !r->body_len) return 0;
    return r->content_type[0] == 0
        || kstrnicmp(r->content_type, "text/html", 9) == 0
        || kstrnicmp(r->content_type, "text/plain", 10) == 0;
}

static void cache_drop(browser_t* b, int i){
    b->cache_bytes -= b->cache[i].resp.body_len;
    http_response_free(&b->cache[i].resp);
    b->cache[i].url[0] = 0;
}

// Copies a cached response for `url` into out. 1 on a hit.
static int cache_lookup(browser_t* b, const char* url, http_response_t* out){
    for (int i = 0; i < PAGE_CACHE_N; i++){
        if (!b->cache[i].url[0] || strcmp(b->cache[i].url, url) != 0) continue;
        if (ticks - b->cache[i].stored_at > PAGE_CACHE_TTL){ cache_drop(b, i); return 0; }

        const http_response_t* c = &b->cache[i].resp;
        uint8_t* body = (uint8_t*)kmalloc(c->body_len + 1);
        if (!body) return 0;
        memcpy(body, c->body, c->body_len);
        body[c->body_len] = 0;
        *out = *c;
        out->body = body;
        b->cache[i].used_at = ++b->cache_clock;
        return 1;
    }
    return 0;
}

static void cache_store(browser_t* b, const char* url, const http_response_t* r){
    if (!cacheable(r) || r->body_len > PAGE_CACHE_BUDGET / 4) return;

    for (int i = 0; i < PAGE_CACHE_N; i++)
        if (b->cache[i].url[0] && strcmp(b->cache[i].url, url) == 0) cache_drop(b, i);

    // Evict least recently used until the body fits and a slot is free.
    for (;;){
        int free_slot = -1, lru = -1;
        for (int i = 0; i < PAGE_CACHE_N; i++){
            if (!b->cache[i].url[0]){ if (free_slot < 0) free_slot = i; continue; }
            if (lru < 0 || b->cache[i].used_at < b->cache[lru].used_at) lru = i;
        }
        if (free_slot >= 0 && b->cache_bytes + r->body_len <= PAGE_CACHE_BUDGET){
            uint8_t* body = (uint8_t*)kmalloc(r->body_len + 1);
            if (!body) return;
            memcpy(body, r->body, r->body_len);
            body[r->body_len] = 0;
            b->cache[free_slot].resp      = *r;
            b->cache[free_slot].resp.body = body;
            b->cache[free_slot].stored_at = ticks;
            b->cache[free_slot].used_at   = ++b->cache_clock;
            strncpy(b->cache[free_slot].url, url, URL_MAX - 1);
            b->cache[free_slot].url[URL_MAX - 1] = 0;
            b->cache_bytes += r->body_len;
            return;
        }
        if (lru < 0) return;
        cache_drop(b, lru);
    }
}

static void fetch_task(void* arg){
    browser_t* b = (browser_t*)arg;

    for (;;){
        if (!b->fetch_go){
            // Between pages, fetch images. One at a time, and the check for a
            // page request comes first on every pass, so a click never waits
            // behind more than the one image already in flight.
            char url[URL_MAX];
            if (img_pop(b, url)){ fetch_image(b, url); continue; }
            task_sleep(50);
            continue;
        }

        int t = b->fetch_tab;
        b->fetch_go = 0;
        if (t < 0 || t >= MAX_TABS){ continue; }

        tab_t* tab = &b->tabs[t];
        http_response_free(&tab->resp);
        memset(&tab->resp, 0, sizeof(tab->resp));

        int fresh = b->fetch_fresh;
        b->fetch_fresh = 0;
        if (!fresh && cache_lookup(b, b->fetch_url, &tab->resp)){
            ksnprintf(tab->status, sizeof(tab->status), "%d  %u bytes  %s  (from memory)",
                      tab->resp.status, tab->resp.body_len,
                      tab->resp.content_type[0] ? tab->resp.content_type : "");
            tab->state = BR_READY;
        } else if (http_get(b->fetch_url, &tab->resp, progress, b) == 0){
            ksnprintf(tab->status, sizeof(tab->status), "%d  %u bytes  %s",
                      tab->resp.status, tab->resp.body_len,
                      tab->resp.content_type[0] ? tab->resp.content_type : "");
            cache_store(b, b->fetch_url, &tab->resp);
            tab->state = BR_READY;
        } else {
            ksnprintf(tab->status, sizeof(tab->status), "%s", tab->resp.error);
            tab->state = BR_ERROR;
        }
        wm_invalidate();
    }
}

static void layout_tab(browser_t* b, tab_t* t, const char* src, uint32_t len,
                       int is_plain, int keep_chrome){
    if (t->page){ html_free(t->page); t->page = 0; }
    int w = b->view_w > 200 ? b->view_w : 200;
    t->page = html_layout_ex(src, len, w, is_plain, keep_chrome);
    t->scroll = 0;

    t->src             = src;
    t->src_len         = len;
    t->src_plain       = is_plain;
    t->src_keep_chrome = keep_chrome;

    if (t->page && t->page->title[0]){
        strncpy(t->title, t->page->title, sizeof(t->title) - 1);
        t->title[sizeof(t->title) - 1] = 0;
    }
}

// A page that laid out to nothing at all.
//
// Nearly always this means the site assembles its content with JavaScript,
// and the markup that arrived really is empty -- YouTube sends the better
// part of a megabyte containing no page text whatsoever. Saying so is the
// whole fix. The previous behaviour was a black rectangle, which reads as a
// broken browser rather than as a page that cannot be shown, and it hides
// the fact that everything underneath -- DNS, TLS, HTTP -- worked.
static void show_empty_notice(browser_t* b, tab_t* t){
    url_t u;
    const char* host = "That site";
    if (url_parse(t->url, &u) == 0 && u.host[0]) host = u.host;

    ksnprintf(b->notice, sizeof(b->notice),
        "<h1>Nothing to render</h1>"
        "<p>%s answered %d and sent %u bytes, and none of it is page content. "
        "Sites built this way put an empty document on the wire and assemble "
        "what you see with JavaScript once it is running in the browser.</p>"
        "<p>hawkOS has no JavaScript engine, so there is nothing here to lay "
        "out. Everything underneath worked: the name resolved, TLS "
        "negotiated, and the server replied.</p>"
        "<h2>Pages that render fully</h2>"
        "<ul>"
        "<li><a href=\"https://en.wikipedia.org/wiki/Operating_system\">Wikipedia</a></li>"
        "<li><a href=\"https://news.ycombinator.com/\">Hacker News</a></li>"
        "<li><a href=\"https://lite.duckduckgo.com/lite/?q=%s\">Search for %s</a></li>"
        "<li><a href=\"https://example.com\">example.com</a></li>"
        "</ul>",
        host, t->resp.status, t->resp.body_len, host, host);

    layout_tab(b, t, b->notice, strlen(b->notice), 0, 0);
    strncpy(t->title, host, sizeof(t->title) - 1);
    t->title[sizeof(t->title) - 1] = 0;
}

static void show_home(browser_t* b, tab_t* t){
    stop_video(t);
    forms_reset(t);
    layout_tab(b, t, HOME_PAGE, strlen(HOME_PAGE), 0, 0);
    strcpy(t->title, "New tab");
    t->url[0] = 0;
    t->url_len = 0;
    ksnprintf(t->status, sizeof(t->status), "%s",
              net_configured() ? "ready" : "no network - DHCP did not complete");
    t->state = BR_IDLE;
}

// Anything with a scheme, or a dotted hostname and no spaces, is a URL.
// Everything else is a search — which is what makes typing two words in the
// address bar do the useful thing instead of failing DNS.
static int looks_like_url(const char* s){
    if (kstrnicmp(s, "http://", 7) == 0 || kstrnicmp(s, "https://", 8) == 0) return 1;
    if (strchr(s, ' ')) return 0;
    const char* dot = strchr(s, '.');
    if (!dot) return 0;
    return dot[1] != 0;               // a trailing dot is not a hostname
}

static void build_search_url(const char* prefix, const char* query, char* out, uint32_t cap){
    uint32_t o = 0;
    const char* pre = prefix;
    while (*pre && o < cap - 1) out[o++] = *pre++;

    static const char HEX[] = "0123456789ABCDEF";
    for (const char* p = query; *p && o + 3 < cap; p++){
        unsigned char c = (unsigned char)*p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '-' || c == '_' || c == '.' || c == '~'){
            out[o++] = (char)c;
        } else if (c == ' '){
            out[o++] = '+';
        } else {
            out[o++] = '%';
            out[o++] = HEX[c >> 4];
            out[o++] = HEX[c & 0x0F];
        }
    }
    out[o] = 0;
}

static void navigate(browser_t* b, const char* url){
    if (!url || !url[0]) return;
    tab_t* t = cur_tab(b);
    if (t->state == BR_LOADING) return;          // one request per tab at a time
    if (b->fetch_go) return;                     // and one in flight overall

    // What the user sees is always the public address, even when the link
    // that got here was resolved against the gateway a page was served from.
    char shown[URL_MAX];
    gateway_to_yt(url, shown, sizeof(shown));

    if (t->history_n < HISTORY_MAX && t->url[0]){
        strncpy(t->history[t->history_n], t->url, URL_MAX - 1);
        t->history[t->history_n][URL_MAX - 1] = 0;
        t->history_scroll[t->history_n] = t->scroll;
        t->history_n++;
    }
    t->pending_scroll = -1;

    strncpy(t->url, shown, URL_MAX - 1);
    t->url[URL_MAX - 1] = 0;
    t->url_len = (int)strlen(t->url);

    fetch_address(t->url, b->fetch_url, URL_MAX);
    stop_video(t);
    forms_reset(t);

    strcpy(t->status, "Starting");
    t->state    = BR_LOADING;
    t->gateway_notice = 0;
    // The fetch is about to replace resp.body, which is what src points at
    // for an already-loaded page. Drop it now so a resize arriving mid-fetch
    // has nothing stale to reflow from; on_tick sets it again on success.
    t->src      = 0;
    t->src_len  = 0;
    if (t->page) t->page->refresh[0] = 0;      // do not re-follow a stale refresh
    b->fetch_tab = b->cur;
    b->fetch_go  = 1;
    wm_invalidate();
}

// Takes whatever is in the address bar and either loads it or searches for it.
static void go(browser_t* b){
    tab_t* t = cur_tab(b);
    char typed[URL_MAX], target[URL_MAX];

    strncpy(typed, t->url, URL_MAX - 1);
    typed[URL_MAX - 1] = 0;
    if (!typed[0]) return;

    if (looks_like_url(typed)){
        if (kstrnicmp(typed, "http://", 7) != 0 && kstrnicmp(typed, "https://", 8) != 0)
            ksnprintf(target, sizeof(target), "https://%s", typed);
        else
            strncpy(target, typed, sizeof(target) - 1);
        target[sizeof(target) - 1] = 0;
    } else if (kstrnicmp(typed, "yt ", 3) == 0 || kstrnicmp(typed, "youtube ", 8) == 0){
        build_search_url(YT_SEARCH, strchr(typed, ' ') + 1, target, sizeof(target));
    } else if (t->resp.final_url[0] && is_gateway(t->resp.final_url)){
        // Words typed while on YouTube search YouTube, the way the search
        // box at the top of every YouTube page would.
        build_search_url(YT_SEARCH, typed, target, sizeof(target));
    } else {
        build_search_url(SEARCH_PREFIX, typed, target, sizeof(target));
    }

    t->url[0] = 0;                                // navigate() pushes the old URL
    navigate(b, target);
}

static void go_back(browser_t* b){
    tab_t* t = cur_tab(b);
    if (t->history_n == 0){ show_home(b, t); return; }
    t->history_n--;
    char prev[URL_MAX];
    strncpy(prev, t->history[t->history_n], URL_MAX - 1);
    prev[URL_MAX - 1] = 0;
    int where = t->history_scroll[t->history_n];
    t->url[0] = 0;
    navigate(b, prev);
    t->pending_scroll = where;
}

static void reload(browser_t* b){
    tab_t* t = cur_tab(b);
    if (!t->url[0]) return;
    char again[URL_MAX];
    strncpy(again, t->url, URL_MAX - 1);
    again[URL_MAX - 1] = 0;
    t->url[0] = 0;
    b->fetch_fresh = 1;
    navigate(b, again);
}

// ---------------------------------------------------------------- tabs

static void tab_reset(tab_t* t){
    stop_video(t);
    if (t->page){ html_free(t->page); t->page = 0; }
    http_response_free(&t->resp);
    memset(t, 0, sizeof(*t));
}

static void new_tab(browser_t* b){
    for (int i = 0; i < MAX_TABS; i++){
        if (b->tabs[i].used) continue;
        tab_reset(&b->tabs[i]);
        b->tabs[i].used = 1;
        b->cur = i;
        show_home(b, &b->tabs[i]);
        b->url_focus = 1;
        b->url_selected = 1;
        return;
    }
}

static void close_tab(browser_t* b, int idx){
    if (idx < 0 || idx >= MAX_TABS || !b->tabs[idx].used) return;

    int live = 0;
    for (int i = 0; i < MAX_TABS; i++) if (b->tabs[i].used) live++;
    if (live <= 1){                     // never leave the window tabless
        tab_reset(&b->tabs[idx]);
        b->tabs[idx].used = 1;
        show_home(b, &b->tabs[idx]);
        return;
    }

    tab_reset(&b->tabs[idx]);
    if (b->cur == idx)
        for (int i = 0; i < MAX_TABS; i++) if (b->tabs[i].used){ b->cur = i; break; }
}

// ---------------------------------------------------------------- painting

static void paint_tabbar(browser_t* b, int x, int y, int w){
    gfx_fill_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, TABBAR_H, TH_PANEL);
    gfx_hline((uint32_t)x, (uint32_t)(y + TABBAR_H - 1), (uint32_t)w, TH_PANEL_EDGE);

    int tx = x + 4;
    for (int i = 0; i < MAX_TABS; i++){
        if (!b->tabs[i].used) continue;
        if (tx + TAB_W > x + w - NEWTAB_W - 8) break;

        // The selected tab is a rounded card lifted out of the strip, rather
        // than the same rectangle in a second colour with a line over it.
        int on = (i == b->cur);
        if (on){
            gfx_fill_round_rect((uint32_t)tx, (uint32_t)(y + 3), TAB_W, TABBAR_H - 3, 7,
                                TH_WIN_BG);
            gfx_fill_rect((uint32_t)tx, (uint32_t)(y + TABBAR_H - 8), TAB_W, 8, TH_WIN_BG);
        } else {
            gfx_vline((uint32_t)(tx + TAB_W - 1), (uint32_t)(y + 8), TABBAR_H - 14,
                      TH_PANEL_EDGE);
        }

        const char* label = b->tabs[i].title[0] ? b->tabs[i].title : "New tab";
        if (b->tabs[i].state == BR_LOADING) label = "Loading...";

        gfx_clip_set((uint32_t)(tx + 8), (uint32_t)y, TAB_W - TAB_CLOSE_W - 24, TABBAR_H);
        gfx_text((uint32_t)(tx + 10), (uint32_t)(y + 9), label,
                 on ? TH_TEXT : TH_TEXT_MUTED, GFX_TRANSPARENT);
        gfx_clip_reset();

        // Per-tab close cross
        int cx = tx + TAB_W - TAB_CLOSE_W - 4, cy = y + 10;
        for (int k = 0; k < 8; k++){
            gfx_put_pixel((uint32_t)(cx + k), (uint32_t)(cy + k), TH_TEXT_DIM);
            gfx_put_pixel((uint32_t)(cx + 7 - k), (uint32_t)(cy + k), TH_TEXT_DIM);
        }

        tx += TAB_W;
    }

    int nx = x + w - NEWTAB_W - 4;
    gfx_fill_round_rect((uint32_t)nx, (uint32_t)(y + 5), NEWTAB_W, TABBAR_H - 11, 5, TH_CONTROL);
    gfx_fill_rect((uint32_t)(nx + 13), (uint32_t)(y + 10), 4, 12, TH_TEXT_MUTED);
    gfx_fill_rect((uint32_t)(nx + 9), (uint32_t)(y + 14), 12, 4, TH_TEXT_MUTED);
}

static void paint_toolbar(browser_t* b, int x, int y, int w){
    tab_t* t = cur_tab(b);

    gfx_fill_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, TOOLBAR_H, TH_PANEL);
    gfx_hline((uint32_t)x, (uint32_t)(y + TOOLBAR_H - 1), (uint32_t)w, TH_PANEL_EDGE);

    int by = y + 7;

    // Back
    int bx = x + 8;
    gfx_fill_round_rect((uint32_t)bx, (uint32_t)by, 32, 24, 5, TH_CONTROL);
    gfx_draw_round_rect((uint32_t)bx, (uint32_t)by, 32, 24, 5, TH_CONTROL_EDGE);
    {
        uint32_t c = t->history_n ? TH_TEXT : TH_TEXT_DIM;
        for (int k = 0; k < 6; k++){
            gfx_put_pixel((uint32_t)(bx + 12 + k), (uint32_t)(by + 12 - k), c);
            gfx_put_pixel((uint32_t)(bx + 12 + k), (uint32_t)(by + 12 + k), c);
        }
        gfx_hline((uint32_t)(bx + 12), (uint32_t)(by + 12), 10, c);
    }

    // Reload
    int rx = x + 46;
    gfx_fill_round_rect((uint32_t)rx, (uint32_t)by, 32, 24, 5, TH_CONTROL);
    gfx_draw_round_rect((uint32_t)rx, (uint32_t)by, 32, 24, 5, TH_CONTROL_EDGE);
    gfx_draw_circle(rx + 16, by + 12, 7, t->url[0] ? TH_TEXT : TH_TEXT_DIM);
    gfx_fill_rect((uint32_t)(rx + 16), (uint32_t)(by + 3), 8, 5, TH_CONTROL);

    // Address field
    int fx = x + 84, fw = w - 84 - 52;
    gfx_fill_round_rect((uint32_t)fx, (uint32_t)by, (uint32_t)fw, 24, 6, TH_FIELD);
    gfx_draw_round_rect((uint32_t)fx, (uint32_t)by, (uint32_t)fw, 24, 6,
                        b->url_focus ? TH_ACCENT : TH_CONTROL_EDGE);

    // A padlock for https, so the security state is visible rather than
    // implied by the text of the URL.
    int text_x = fx + 8;
    if (kstrnicmp(t->url, "https://", 8) == 0){
        gfx_fill_rect((uint32_t)(fx + 7), (uint32_t)(by + 11), 8, 7, TH_OK);
        gfx_draw_circle(fx + 11, by + 9, 3, TH_OK);
        text_x = fx + 22;
    }

    int cols = (fx + fw - 6 - text_x) / FONT_W;
    if (cols < 4) cols = 4;
    int start = t->url_len > cols ? t->url_len - cols : 0;

    if (b->url_focus && b->url_selected)
        gfx_fill_rect((uint32_t)(text_x - 2), (uint32_t)(by + 3),
                      (uint32_t)(fx + fw - text_x), 18, TH_SELECT);

    gfx_clip_set((uint32_t)(fx + 2), (uint32_t)by, (uint32_t)(fw - 4), 24);
    if (t->url_len == 0 && !b->url_focus)
        gfx_text((uint32_t)text_x, (uint32_t)(by + 4),
                 "Search or enter address", TH_TEXT_DIM, GFX_TRANSPARENT);
    else
        gfx_text_n((uint32_t)text_x, (uint32_t)(by + 4), t->url + start,
                   (uint32_t)(t->url_len - start), TH_TEXT, GFX_TRANSPARENT);
    if (b->url_focus)
        gfx_fill_rect((uint32_t)(text_x + (t->url_len - start) * FONT_W), (uint32_t)(by + 3),
                      2, 18, TH_ACCENT);
    gfx_clip_reset();

    // Go
    int gx = x + w - 46;
    gfx_fill_round_rect((uint32_t)gx, (uint32_t)by, 38, 24, 6, TH_ACCENT);
    gfx_text((uint32_t)(gx + 11), (uint32_t)(by + 4), "Go", TH_ACCENT_TEXT, GFX_TRANSPARENT);
}

static void paint_status(browser_t* b, int x, int y, int w){
    tab_t* t = cur_tab(b);

    gfx_fill_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, STATUS_H, TH_PANEL);
    gfx_hline((uint32_t)x, (uint32_t)y, (uint32_t)w, TH_PANEL_EDGE);

    uint32_t col = (t->state == BR_ERROR) ? TH_ERROR : TH_TEXT_MUTED;
    const char* line = t->status;
    char vline[128];
    if (t->video && t->state != BR_LOADING){
        char pos[16];
        clock_text(pos, sizeof(pos), video_where(t));
        // CPU load over the last second or so: frames per second alone
        // cannot show headroom, since it stops at the video's own rate.
        static uint32_t li0, la0, load;
        uint32_t li, la;
        sched_load(&li, &la);
        if (la - la0 >= 100){
            load = 100u - ((li - li0) * 100u) / (la - la0);
            li0 = li; la0 = la;
        }
        ksnprintf(vline, sizeof(vline), "video: %s  %s  %dp%s  %u fps  cpu %u%%  %u KB buffered",
                  video_status(t->video), pos, b->quality, b->quality_manual ? "" : " auto",
                  video_fps(t->video), load, video_buffered_kb(t->video));
        line = vline;
        if (video_state(t->video) == VIDEO_ERROR) col = TH_ERROR;
    }
    gfx_clip_set((uint32_t)x, (uint32_t)y, (uint32_t)(w - 150), STATUS_H);
    gfx_text((uint32_t)(x + 10), (uint32_t)(y + 4), line, col, GFX_TRANSPARENT);
    gfx_clip_reset();

    // DNS server, so a lookup failure is diagnosable without leaving the app.
    char right[48];
    if (net_configured()){
        char dns[16];
        ksnprintf(right, sizeof(right), "dns %s", net_ip_str(net_dns_server(), dns));
    } else {
        strcpy(right, "offline");
    }

    int rx = x + w - 10 - (int)gfx_text_width(right);
    if (t->page && t->page->height > 0){
        char pos[16];
        int pct = t->page->height <= b->view_h ? 100
                : (int)(((int64_t)t->scroll + b->view_h) * 100 / t->page->height);
        if (pct > 100) pct = 100;
        ksnprintf(pos, sizeof(pos), "%d%%", pct);
        rx -= (int)gfx_text_width(pos) + 16;
        gfx_text((uint32_t)(x + w - 10 - (int)gfx_text_width(pos)), (uint32_t)(y + 4),
                 pos, TH_TEXT_DIM, GFX_TRANSPARENT);
    }
    gfx_text((uint32_t)rx, (uint32_t)(y + 4), right, TH_TEXT_DIM, GFX_TRANSPARENT);
}

// ------------------------------------------------------------------ forms

static void forms_reset(tab_t* t){
    t->edit_n = 0;
    t->field_focus = -1;
}

// The edit buffer for field `f`, created from the field's own value the
// first time it is typed into.
static char* field_text(tab_t* t, int f, int create){
    for (int i = 0; i < t->edit_n; i++) if (t->edits[i].field == f) return t->edits[i].text;
    if (!create || t->edit_n >= 8 || !t->page) return 0;
    const html_field_t* fd = &t->page->fields[f];
    t->edits[t->edit_n].field = f;
    html_slice(t->page, fd->value_off, fd->value_len, t->edits[t->edit_n].text,
               sizeof(t->edits[0].text));
    return t->edits[t->edit_n++].text;
}

static void url_encode_into(const char* s, char* out, uint32_t* o, uint32_t cap){
    static const char HEX[] = "0123456789ABCDEF";
    for (; *s && *o + 4 < cap; s++){
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == '-' || c == '_' || c == '.' || c == '~') out[(*o)++] = (char)c;
        else if (c == ' ') out[(*o)++] = '+';
        else { out[(*o)++] = '%'; out[(*o)++] = HEX[c >> 4]; out[(*o)++] = HEX[c & 15]; }
    }
    out[*o] = 0;
}

static void navigate(browser_t* b, const char* url);

// Submits the form that field `from` belongs to, as a GET: the action
// resolved against the page, then every named text and hidden field of the
// form, and the submit button if that is what was pressed.
static void form_submit(browser_t* b, tab_t* t, int from){
    html_page_t* p = t->page;
    if (!p || from < 0 || (uint32_t)from >= p->field_n) return;
    int form = p->fields[from].form;

    char action[URL_MAX], base_url[URL_MAX], target[URL_MAX];
    action[0] = 0;
    if (form >= 0 && (uint32_t)form < p->form_n)
        html_slice(p, p->forms[form].action_off, p->forms[form].action_len, action, sizeof(action));

    strncpy(base_url, t->resp.final_url[0] ? t->resp.final_url : t->url, sizeof(base_url) - 1);
    base_url[sizeof(base_url) - 1] = 0;
    url_t base;
    if (url_parse(base_url, &base) != 0) return;
    if (action[0]) url_resolve(&base, action, target, sizeof(target));
    else           strncpy(target, base_url, sizeof(target) - 1);
    target[sizeof(target) - 1] = 0;
    char* q = strchr(target, '?');
    if (q) *q = 0;

    uint32_t o = strlen(target);
    int first = 1;
    for (uint32_t i = 0; i < p->field_n; i++){
        const html_field_t* f = &p->fields[i];
        if (f->form != form || !f->name_len) continue;
        if (f->kind == HTML_FIELD_SUBMIT && (int)i != from) continue;
        char name[64], value[256];
        html_slice(p, f->name_off, f->name_len, name, sizeof(name));
        const char* edited = field_text(t, (int)i, 0);
        if (edited) strncpy(value, edited, sizeof(value) - 1), value[sizeof(value) - 1] = 0;
        else        html_slice(p, f->value_off, f->value_len, value, sizeof(value));
        if (o + 2 >= sizeof(target)) break;
        target[o++] = first ? '?' : '&';
        first = 0;
        url_encode_into(name, target, &o, sizeof(target));
        if (o + 2 >= sizeof(target)) break;
        target[o++] = '=';
        url_encode_into(value, target, &o, sizeof(target));
    }
    target[o] = 0;
    t->field_focus = -1;
    navigate(b, target);
}

// What has been typed into the page's fields, over the placeholder text the
// layout drew in them, with a caret in the one that has the keyboard.
static void paint_fields(browser_t* b, tab_t* t, int x, int vy){
    html_page_t* p = t->page;
    for (int i = 0; i < t->edit_n; i++){
        int f = t->edits[i].field;
        if (f < 0 || (uint32_t)f >= p->field_n || p->fields[f].box < 0) continue;
        const html_box_t* bx = &p->boxes[p->fields[f].box];
        int sx = x + bx->x, sy = vy + bx->y - t->scroll;
        if (sy < vy || sy + bx->h > vy + b->view_h) continue;
        int on = (f == t->field_focus);
        gfx_fill_rect((uint32_t)sx, (uint32_t)sy, (uint32_t)bx->w, (uint32_t)bx->h, TH_PAGE_FIELD_BG);
        gfx_draw_rect((uint32_t)sx, (uint32_t)sy, (uint32_t)bx->w, (uint32_t)bx->h,
                      on ? TH_ACCENT : TH_PAGE_FIELD);
        const char* s = t->edits[i].text;
        uint32_t n = strlen(s);
        // Keep the end of the text, where the caret is, in view.
        while (n && (int)gfx_pf_width(s, &pf_body) > bx->w - 24){ s++; n--; }
        gfx_clip_set((uint32_t)(sx + 2), (uint32_t)sy, (uint32_t)(bx->w - 4), (uint32_t)bx->h);
        uint32_t adv = gfx_pf_text_n((uint32_t)(sx + 10), (uint32_t)(sy + 2), s, n, &pf_body, TH_PAGE_TEXT);
        if (on) gfx_fill_rect((uint32_t)(sx + 10 + (int)adv + 1), (uint32_t)(sy + 5), 2,
                              (uint32_t)(bx->h - 10), TH_ACCENT);
        gfx_clip_set((uint32_t)x, (uint32_t)vy, (uint32_t)b->view_w, (uint32_t)b->view_h);
    }
}

// The largest rectangle of the source's shape that fits the box.
static void fit_rect(int sw, int sh, int bw, int bh, int* dw, int* dh){
    if ((int64_t)sw * bh > (int64_t)sh * bw){ *dw = bw; *dh = (int)((int64_t)sh * bw / sw); }
    else                                     { *dh = bh; *dw = (int)((int64_t)sw * bh / sh); }
    if (*dw < 1) *dw = 1;
    if (*dh < 1) *dh = 1;
}

// The gfx rectangle calls take unsigned coordinates, so anything that may sit
// partly above the viewport is trimmed to it here first.
static void fill_in_view(int x, int y, int w, int h, int vy, int vh, uint32_t c, int alpha){
    if (y < vy){ h -= vy - y; y = vy; }
    if (y + h > vy + vh) h = vy + vh - y;
    if (w <= 0 || h <= 0 || x < 0) return;
    if (alpha >= 255) gfx_fill_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, c);
    else              gfx_blend_rect((uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h, c, (uint32_t)alpha);
}

typedef struct { int x, y, w, h; } rect_t;

static int in_rect(const rect_t* r, int x, int y){
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

// Where the current tab's video is, for a page view whose top-left corner is
// at (ox, oy): the same function places it for painting (screen coordinates)
// and for hit-testing clicks (client coordinates).
static int video_rect(browser_t* b, tab_t* t, int ox, int oy, rect_t* r){
    if (!t->video || !t->page || t->video_box < 0 || (uint32_t)t->video_box >= t->page->box_n)
        return 0;
    if (b->fullscreen){
        r->x = ox; r->y = oy; r->w = b->view_w; r->h = b->view_h;
        return 1;
    }
    const html_box_t* bx = &t->page->boxes[t->video_box];
    r->x = ox + bx->x;
    r->y = oy + bx->y - t->scroll;
    r->w = bx->w;
    r->h = bx->h;
    return 1;
}

typedef struct {
    rect_t bar;             // the translucent strip the controls sit on
    rect_t seek;            // clickable band around the progress line
    rect_t play;
    rect_t quality[QUALITY_N];
    rect_t fs;
    int    time_x, vol_x;
} controls_t;

static void controls_layout(const rect_t* r, controls_t* c){
    c->bar  = (rect_t){ r->x, r->y + r->h - CTL_H, r->w, CTL_H };
    int y = c->bar.y + 5;
    c->play = (rect_t){ r->x + 6, y, 30, 24 };
    c->fs   = (rect_t){ r->x + r->w - 36, y, 30, 24 };
    int qx = c->fs.x - 6 - QUALITY_N * 46;
    for (int i = 0; i < QUALITY_N; i++) c->quality[i] = (rect_t){ qx + i * 46, y, 44, 24 };
    c->seek   = (rect_t){ r->x + 8, c->bar.y - 12, r->w - 16, 14 };
    c->time_x = r->x + 44;
    c->vol_x  = qx - 80;
}

static void clock_text(char* out, uint32_t cap, uint32_t ms){
    uint32_t s = ms / 1000u;
    if (s >= 3600) ksnprintf(out, cap, "%u:%02u:%02u", s / 3600u, (s / 60u) % 60u, s % 60u);
    else           ksnprintf(out, cap, "%u:%02u", s / 60u, s % 60u);
}

static void paint_controls(browser_t* b, tab_t* t, const rect_t* r, int vy){
    controls_t c;
    controls_layout(r, &c);
    // The controls draw only when wholly inside the page view: the gfx
    // rectangle calls take unsigned coordinates.
    if (c.seek.y < vy || c.bar.y + CTL_H > vy + b->view_h) return;

    video_t* v = t->video;
    const uint32_t white = GFX_RGB(255, 255, 255), dim = GFX_RGB(170, 170, 170);
    const uint32_t red = GFX_RGB(230, 33, 23);

    gfx_blend_rect((uint32_t)r->x, (uint32_t)(c.seek.y - 4), (uint32_t)r->w,
                   (uint32_t)(r->y + r->h - c.seek.y + 4), GFX_RGB(0, 0, 0), 150);

    // Progress: the played part in red over a grey track, and a knob.
    uint32_t dur = video_duration_ms(v), pos = video_where(t);
    int ly = c.seek.y + c.seek.h / 2 - 2;
    gfx_fill_rect((uint32_t)c.seek.x, (uint32_t)ly, (uint32_t)c.seek.w, 4, GFX_RGB(110, 110, 110));
    if (dur){
        int done = (int)(((uint64_t)(pos > dur ? dur : pos) * (uint32_t)c.seek.w) / dur);
        gfx_fill_rect((uint32_t)c.seek.x, (uint32_t)ly, (uint32_t)done, 4, red);
        gfx_fill_circle(c.seek.x + done, ly + 2, 6, red);
    }

    // Play / pause.
    int st = video_state(v);
    int px0 = c.play.x + 9, py0 = c.play.y + 5;
    if (st == VIDEO_PLAYING || st == VIDEO_BUFFERING || st == VIDEO_CONNECTING){
        gfx_fill_rect((uint32_t)px0, (uint32_t)py0, 4, 14, white);
        gfx_fill_rect((uint32_t)(px0 + 8), (uint32_t)py0, 4, 14, white);
    } else {
        for (int k = 0; k < 14; k++){
            int half = k < 7 ? k : 13 - k;
            gfx_hline((uint32_t)px0, (uint32_t)(py0 + k), (uint32_t)(half + 1) * 2, white);
        }
    }

    char now[16], total[16], line[40];
    clock_text(now, sizeof(now), pos);
    if (dur){ clock_text(total, sizeof(total), dur); ksnprintf(line, sizeof(line), "%s / %s", now, total); }
    else      ksnprintf(line, sizeof(line), "%s", now);
    gfx_text((uint32_t)c.time_x, (uint32_t)(c.play.y + 4), line, white, GFX_TRANSPARENT);

    if (c.vol_x > c.time_x + (int)gfx_text_width(line) + 12){
        char vol[16];
        if (b->muted) ksnprintf(vol, sizeof(vol), "muted");
        else          ksnprintf(vol, sizeof(vol), "vol %d%%", b->volume);
        gfx_text((uint32_t)c.vol_x, (uint32_t)(c.play.y + 4), vol, dim, GFX_TRANSPARENT);
    }

    for (int i = 0; i < QUALITY_N; i++){
        const rect_t* q = &c.quality[i];
        char lab[8];
        ksnprintf(lab, sizeof(lab), "%dp", QUALITY[i]);
        int on = (QUALITY[i] == b->quality);
        if (on) gfx_fill_round_rect((uint32_t)q->x, (uint32_t)q->y, (uint32_t)q->w, (uint32_t)q->h, 5, red);
        int tw = (int)gfx_text_width(lab);
        gfx_text((uint32_t)(q->x + (q->w - tw) / 2), (uint32_t)(q->y + 4), lab,
                 on ? white : dim, GFX_TRANSPARENT);
    }

    // Fullscreen: four corner brackets, pointing out (enter) or in (leave).
    int fx = c.fs.x + 7, fy = c.fs.y + 5, fw = 16, fh = 14, a = 5;
    int in = b->fullscreen;
    for (int k = 0; k < 4; k++){
        int cx = (k & 1) ? fx + fw - 2 : fx, cy = (k & 2) ? fy + fh - 2 : fy;
        int hx = (k & 1) ? cx - a + 2 : cx, vyy = (k & 2) ? cy - a + 2 : cy;
        if (in){ hx = (k & 1) ? cx : cx - a + 2; vyy = (k & 2) ? cy : cy - a + 2; }
        gfx_fill_rect((uint32_t)hx, (uint32_t)cy, (uint32_t)a, 2, white);
        gfx_fill_rect((uint32_t)cx, (uint32_t)vyy, 2, (uint32_t)a, white);
    }
}

static void paint_video(browser_t* b, tab_t* t, uint32_t i, int sx, int sy, int w, int h, int vy){
    video_t* v = t->video;
    if (!v || (int)i != t->video_box) return;

    video_picture_t pic;
    uint32_t serial;
    int have = video_picture(v, &pic, &serial);
    if (!have && t->video_prev) have = video_picture(t->video_prev, &pic, &serial);
    if (have){
        int dw, dh;
        fit_rect(pic.w, pic.h, w, h, &dw, &dh);
        gfx_blit_yuv(sx + (w - dw) / 2, sy + (h - dh) / 2, dw, dh,
                     pic.y, pic.cb, pic.cr, pic.w, pic.h, pic.ystride, pic.cstride);
    }

    int st = video_state(v);
    const int white = (int)GFX_RGB(255, 255, 255);

    // Anything but plain playback says so in the middle of the picture.
    if (st != VIDEO_PLAYING){
        const char* msg = video_status(v);
        char buf[112];
        if (st == VIDEO_PAUSED)      msg = "Paused";
        else if (st == VIDEO_ENDED)  msg = "Ended";
        ksnprintf(buf, sizeof(buf), "%s", msg);
        int tw = (int)gfx_text_width(buf) + 24;
        int bx = sx + (w - tw) / 2, by = sy + h / 2 - 16;
        if (by >= vy && by + 32 <= vy + b->view_h){
            fill_in_view(bx, by, tw, 32, vy, b->view_h, GFX_RGB(0, 0, 0), 170);
            gfx_text((uint32_t)(bx + 12), (uint32_t)(by + 8), buf,
                     st == VIDEO_ERROR ? GFX_RGB(255, 120, 110) : (uint32_t)white, GFX_TRANSPARENT);
        }
    }

    rect_t r = { sx, sy, w, h };
    paint_controls(b, t, &r, vy);
}

// Decoded images and the playing video, drawn over the frames html_paint
// left for them.
static void paint_media(browser_t* b, tab_t* t, int x, int vy){
    html_page_t* p = t->page;
    for (uint32_t i = 0; i < p->box_n; i++){
        const html_box_t* bx = &p->boxes[i];
        if (bx->kind != HTML_BOX_FRAME && bx->kind != HTML_BOX_VIDEO) continue;
        int sx = x + bx->x, sy = vy + bx->y - t->scroll;
        if (sy + bx->h < vy || sy > vy + b->view_h) continue;

        if (bx->kind == HTML_BOX_VIDEO){
            paint_video(b, t, i, sx, sy, bx->w, bx->h, vy);
            continue;
        }
        if (!bx->src_len) continue;

        char abs[URL_MAX];
        if (box_abs_src(t, i, abs, sizeof(abs)) != 0) continue;
        const image_t* img = 0;
        if (image_cache_get(abs, &img) != IMG_READY || !img) continue;

        int dw, dh;
        fit_rect(img->w, img->h, bx->w, bx->h, &dw, &dh);
        fill_in_view(sx, sy, bx->w, bx->h, vy, b->view_h, TH_PAGE_BG, 255);
        gfx_blit_scaled(sx + (bx->w - dw) / 2, sy + (bx->h - dh) / 2, dw, dh,
                        img->px, img->w, img->h, img->w);
    }
}

static void paint_scrollbar(browser_t* b, int x, int y, int h){
    tab_t* t = cur_tab(b);

    gfx_fill_rect((uint32_t)x, (uint32_t)y, SCROLLBAR_W, (uint32_t)h, TH_PAGE_BG);

    if (!t->page || t->page->height <= h) return;

    int thumb = h * h / t->page->height;
    if (thumb < 24) thumb = 24;
    int span = h - thumb;
    int max_scroll = t->page->height - h;
    int pos = max_scroll > 0 ? (t->scroll * span / max_scroll) : 0;

    gfx_fill_round_rect((uint32_t)(x + 3), (uint32_t)(y + pos), SCROLLBAR_W - 6, (uint32_t)thumb,
                        3, TH_TEXT_DIM);
}

// WM_EV_VIDEO: repaint only the video (picture and controls), clipped to the
// page view, and hand the rectangle back to the compositor.
static void paint_video_only(wm_window_t* win, browser_t* b){
    tab_t* t = cur_tab(b);
    if (!t->video || t->state == BR_LOADING) return;

    int x, y, w, h;
    wm_client_rect(win, &x, &y, &w, &h);
    int vy = y + TABBAR_H + TOOLBAR_H;
    rect_t r;
    if (!video_rect(b, t, x, vy, &r)) return;

    int x0 = r.x, y0 = r.y, x1 = r.x + r.w, y1 = r.y + r.h;
    if (x0 < x) x0 = x;
    if (y0 < vy) y0 = vy;
    if (x1 > x + b->view_w) x1 = x + b->view_w;
    if (y1 > vy + b->view_h) y1 = vy + b->view_h;
    if (x0 >= x1 || y0 >= y1) return;

    // Black first: the picture is letterboxed inside the area, and the bars
    // either side of it are part of what is being repainted.
    gfx_clip_set((uint32_t)x0, (uint32_t)y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0));
    gfx_fill_rect((uint32_t)x0, (uint32_t)y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0), GFX_RGB(0, 0, 0));
    paint_video(b, t, (uint32_t)t->video_box, r.x, r.y, r.w, r.h, vy);
    gfx_clip_reset();
    wm_region_painted(x0, y0, x1 - x0, y1 - y0);
}

// The selection in document order, or a.run == -1 when there is none.
static void sel_range(browser_t* b, tab_t* t, html_pos_t* a, html_pos_t* e){
    a->run = e->run = -1; a->off = e->off = 0;
    if (!t->page || b->sel_page != t->page || b->sel_anchor.run < 0 || b->sel_head.run < 0) return;
    if (html_pos_cmp(b->sel_anchor, b->sel_head) == 0) return;
    if (html_pos_cmp(b->sel_anchor, b->sel_head) < 0){ *a = b->sel_anchor; *e = b->sel_head; }
    else                                              { *a = b->sel_head;   *e = b->sel_anchor; }
}

static void copy_page_selection(browser_t* b, tab_t* t){
    html_pos_t a, e;
    sel_range(b, t, &a, &e);
    if (a.run < 0) return;
    uint32_t cap = CLIP_TEXT_MAX;
    char* buf = (char*)kmalloc(cap);
    if (!buf) return;
    uint32_t n = html_selection_text(t->page, a, e, buf, cap);
    if (n) clip_set_text(buf, n);
    kfree(buf);
}

static void paint(wm_window_t* win, browser_t* b){
    int x, y, w, h;
    wm_client_rect(win, &x, &y, &w, &h);
    b->win = win;

    b->view_w = w - SCROLLBAR_W;
    b->view_h = h - TABBAR_H - TOOLBAR_H - STATUS_H;

    paint_tabbar(b, x, y, w);
    paint_toolbar(b, x, y + TABBAR_H, w);

    tab_t* t = cur_tab(b);
    int vy = y + TABBAR_H + TOOLBAR_H;

    gfx_fill_rect((uint32_t)x, (uint32_t)vy, (uint32_t)b->view_w, (uint32_t)b->view_h, TH_PAGE_BG);

    gfx_clip_set((uint32_t)x, (uint32_t)vy, (uint32_t)b->view_w, (uint32_t)b->view_h);
    if (t->state == BR_LOADING){
        gfx_text_scaled((uint32_t)(x + 20), (uint32_t)(vy + 24), "Loading",
                        TH_TEXT_MUTED, GFX_TRANSPARENT, 2);
        gfx_text((uint32_t)(x + 20), (uint32_t)(vy + 66), t->status, TH_TEXT_DIM, GFX_TRANSPARENT);
    } else if (b->fullscreen && t->video){
        rect_t r;
        gfx_fill_rect((uint32_t)x, (uint32_t)vy, (uint32_t)b->view_w, (uint32_t)b->view_h, GFX_RGB(0, 0, 0));
        if (video_rect(b, t, x, vy, &r)) paint_video(b, t, (uint32_t)t->video_box, r.x, r.y, r.w, r.h, vy);
    } else if (t->page){
        html_pos_t sa = { -1, 0 }, sb = { -1, 0 };
        sel_range(b, t, &sa, &sb);
        html_paint_sel(t->page, x, vy, b->view_w, b->view_h, t->scroll, sa, sb);
        paint_media(b, t, x, vy);
        paint_fields(b, t, x, vy);
    }
    gfx_clip_reset();

    paint_scrollbar(b, x + b->view_w, vy, b->view_h);
    paint_status(b, x, y + h - STATUS_H, w);
}

// ----------------------------------------------------------------- input

static void clamp_scroll(browser_t* b){
    tab_t* t = cur_tab(b);
    int max = t->page ? t->page->height - b->view_h : 0;
    if (max < 0) max = 0;
    if (t->scroll > max) t->scroll = max;
    if (t->scroll < 0) t->scroll = 0;
}

// Ctrl+A / C / X / V. They act on whatever has the keyboard: the address bar,
// a field on the page, or -- when neither does -- the text selected on the page.
// The address bar and page fields only ever edit at their end, so "select all"
// on them is the whole text and cut and copy are the whole text.
static void edit_clipboard(browser_t* b, tab_t* t, int k){
    char* text = 0;
    int cap = 0;
    int is_url = b->url_focus;

    if (is_url){
        text = t->url;
        cap = URL_MAX - 2;
    } else if (t->field_focus >= 0 && t->page && (uint32_t)t->field_focus < t->page->field_n){
        text = field_text(t, t->field_focus, 1);
        cap = (int)sizeof(t->edits[0].text) - 1;
    }

    if (!text){
        if (k == 'c') copy_page_selection(b, t);
        else if (k == 'a' && t->page){
            b->sel_page   = t->page;
            b->sel_anchor = html_text_start(t->page);
            b->sel_head   = html_text_end(t->page);
        }
        return;
    }

    int n = (int)strlen(text);
    switch (k){
        case 'a': if (is_url) b->url_selected = 1; break;
        case 'c': clip_set_text(text, (uint32_t)n); break;
        case 'x':
            clip_set_text(text, (uint32_t)n);
            text[0] = 0;
            if (is_url){ t->url_len = 0; b->url_selected = 0; }
            break;
        case 'v': {
            uint32_t cn;
            const char* c = clip_text(&cn);
            if (is_url && b->url_selected){ n = 0; text[0] = 0; b->url_selected = 0; }
            for (uint32_t i = 0; i < cn && n < cap; i++){
                if (c[i] == '\n' || c[i] == '\r') break;
                if (c[i] >= 32 && c[i] <= 126) text[n++] = c[i];
            }
            text[n] = 0;
            if (is_url) t->url_len = n;
        } break;
        default: break;
    }
}

static void on_key(browser_t* b, int key){
    tab_t* t = cur_tab(b);

    if (kbd_ctrl_down()){
        if (key == 't' || key == 'T'){ new_tab(b); return; }
        if (key == 'w' || key == 'W'){ close_tab(b, b->cur); return; }
        if (key == 'r' || key == 'R'){ reload(b); return; }
        if (key == 'l' || key == 'L'){ b->url_focus = 1; b->url_selected = 1; return; }

        int k = (key >= 'A' && key <= 'Z') ? key + 32 : key;
        if (k == 'a' || k == 'c' || k == 'x' || k == 'v'){
            edit_clipboard(b, t, k);
            return;
        }
    }

    if (b->url_focus){
        if (key == '\n'){ b->url_focus = 0; b->url_selected = 0; go(b); return; }
        if (key == 27){ b->url_focus = 0; b->url_selected = 0; return; }
        if (key == '\b'){
            if (b->url_selected){ t->url[0] = 0; t->url_len = 0; b->url_selected = 0; return; }
            if (t->url_len) t->url[--t->url_len] = 0;
            return;
        }
        if (key >= 32 && key <= 126 && t->url_len < URL_MAX - 2){
            if (b->url_selected){ t->url_len = 0; b->url_selected = 0; }
            t->url[t->url_len++] = (char)key;
            t->url[t->url_len] = 0;
        }
        return;
    }

    // A page field with the keyboard takes typing before the page does.
    if (t->field_focus >= 0 && t->page && (uint32_t)t->field_focus < t->page->field_n){
        char* text = field_text(t, t->field_focus, 1);
        if (text){
            uint32_t n = strlen(text);
            if (key == '\n'){ form_submit(b, t, t->field_focus); return; }
            if (key == 27){ t->field_focus = -1; return; }
            if (key == '\b'){ if (n) text[n - 1] = 0; return; }
            if (key >= 32 && key <= 126){
                if (n + 1 < sizeof(t->edits[0].text)){ text[n] = (char)key; text[n + 1] = 0; }
                return;
            }
        }
    }

    // The player's keys, the ones YouTube itself uses where there is one.
    if (t->video){
        switch (key){
            case ' ': case 'k': case 'K': toggle_play(b, t); return;
            case KEY_LEFT:  video_seek(b, t, (int64_t)video_where(t) - SEEK_STEP_MS); return;
            case KEY_RIGHT: video_seek(b, t, (int64_t)video_where(t) + SEEK_STEP_MS); return;
            case 'f': case 'F': set_fullscreen(b, !b->fullscreen); return;
            case 27:  if (b->fullscreen){ set_fullscreen(b, 0); return; } break;
            case 'm': case 'M': b->muted = !b->muted; apply_volume(b); return;
            case '+': case '=':
                b->volume = b->volume + 10 > 100 ? 100 : b->volume + 10;
                b->muted = 0; apply_volume(b); return;
            case '-': case '_':
                b->volume = b->volume - 10 < 0 ? 0 : b->volume - 10;
                apply_volume(b); return;
            case '1': case '2': case '3': case '4': case '5':
                set_quality(b, t, QUALITY[key - '1']); return;
            case KEY_HOME: video_seek(b, t, 0); return;
            default: break;
        }
        // In fullscreen there is no page to scroll: Up and Down (and so the
        // mouse wheel) are volume, in small steps.
        if (b->fullscreen && (key == KEY_UP || key == KEY_DOWN)){
            int step = key == KEY_UP ? 5 : -5;
            b->volume += step;
            if (b->volume > 100) b->volume = 100;
            if (b->volume < 0)   b->volume = 0;
            b->muted = 0;
            apply_volume(b);
            return;
        }
    }

    switch (key){
        case KEY_DOWN:  t->scroll += 48;             break;
        case KEY_UP:    t->scroll -= 48;             break;
        case KEY_PGDN:  t->scroll += b->view_h - 40; break;
        case KEY_PGUP:  t->scroll -= b->view_h - 40; break;
        case KEY_HOME:  t->scroll = 0;               break;
        case KEY_END:   t->scroll = t->page ? t->page->height : 0; break;
        case '\b':      go_back(b);                  return;
        case ' ':       t->scroll += b->view_h - 40;  break;
        case '\n':      b->url_focus = 1; b->url_selected = 1; return;
        default: break;
    }
    clamp_scroll(b);
}

static void follow_link(browser_t* b, tab_t* t, int idx){
    char href[URL_MAX], abs[URL_MAX];
    if (html_link_href(t->page, idx, href, sizeof(href)) != 0) return;

    // Relative links have to be resolved against the page they came from,
    // which is why the response carries the URL it finally settled on rather
    // than the one that was requested.
    url_t base;
    if (t->resp.final_url[0] && url_parse(t->resp.final_url, &base) == 0){
        url_resolve(&base, href, abs, sizeof(abs));
    } else {
        strncpy(abs, href, sizeof(abs) - 1);
        abs[sizeof(abs) - 1] = 0;
    }

    char direct[URL_MAX];
    t->refresh_hops = 0;
    if (url_unwrap(abs, direct, sizeof(direct)) == 0) navigate(b, direct);
    else                                              navigate(b, abs);
}

static void on_click(wm_window_t* win, browser_t* b, int cx, int cy){
    int x, y, w, h;
    wm_client_rect(win, &x, &y, &w, &h);
    (void)x; (void)y;

    b->sel_page = 0;             // any press begins by dropping the old selection
    b->press_link = -1;

    if (cy < TABBAR_H){
        int nx = w - NEWTAB_W - 4;
        if (cx >= nx){ new_tab(b); return; }

        int tx = 4;
        for (int i = 0; i < MAX_TABS; i++){
            if (!b->tabs[i].used) continue;
            if (tx + TAB_W > w - NEWTAB_W - 8) break;
            if (cx >= tx && cx < tx + TAB_W){
                if (cx >= tx + TAB_W - TAB_CLOSE_W - 8) close_tab(b, i);
                else { b->cur = i; b->url_focus = 0; }
                return;
            }
            tx += TAB_W;
        }
        return;
    }

    if (cy < TABBAR_H + TOOLBAR_H){
        if (cx >= 8 && cx < 40){ go_back(b); return; }
        if (cx >= 46 && cx < 78){ reload(b); return; }
        if (cx >= w - 46){ b->url_focus = 0; b->url_selected = 0; go(b); return; }
        if (cx >= 84 && cx < w - 52){ b->url_focus = 1; b->url_selected = 1; return; }
        return;
    }

    if (cy >= h - STATUS_H) return;

    tab_t* t = cur_tab(b);
    int vy = TABBAR_H + TOOLBAR_H;

    if (cx >= b->view_w){                                    // scrollbar
        b->drag_scroll = 1;
        int rel = cy - vy;
        if (t->page && t->page->height > b->view_h)
            t->scroll = rel * (t->page->height - b->view_h) / b->view_h;
        clamp_scroll(b);
        return;
    }

    b->url_focus = 0;
    b->url_selected = 0;

    if (!t->page) return;

    int py = cy - vy + t->scroll;
    rect_t vr;
    if (video_rect(b, t, 0, vy, &vr)){
        controls_t c;
        controls_layout(&vr, &c);
        uint32_t dur = video_duration_ms(t->video);
        if (dur && in_rect(&c.seek, cx, cy)){
            int rel = cx - c.seek.x;
            if (rel < 0) rel = 0;
            if (rel > c.seek.w) rel = c.seek.w;
            video_seek(b, t, (int64_t)((uint64_t)dur * (uint32_t)rel / (uint32_t)c.seek.w));
            return;
        }
        if (in_rect(&c.bar, cx, cy)){
            if (in_rect(&c.play, cx, cy)) toggle_play(b, t);
            else if (in_rect(&c.fs, cx, cy)) set_fullscreen(b, !b->fullscreen);
            else for (int i = 0; i < QUALITY_N; i++)
                if (in_rect(&c.quality[i], cx, cy)) set_quality(b, t, QUALITY[i]);
            return;
        }
        if (in_rect(&vr, cx, cy)){ toggle_play(b, t); return; }
    }
    if (b->fullscreen) return;

    int fi = html_field_at(t->page, cx, py);
    if (fi >= 0){
        if (t->page->fields[fi].kind == HTML_FIELD_SUBMIT) form_submit(b, t, fi);
        else { t->field_focus = fi; field_text(t, fi, 1); }
        return;
    }
    t->field_focus = -1;

    // A press on the page starts a selection and, if it landed on a link,
    // remembers the link: it is followed on release, provided the pointer did
    // not travel -- so dragging across a link's text selects it instead of
    // navigating away from it.
    b->sel_page   = t->page;
    b->sel_anchor = b->sel_head = html_text_pos(t->page, cx, py);
    b->sel_drag   = 1;
    b->sel_moved  = 0;
    b->press_x    = cx;
    b->press_y    = cy;
    b->press_link = html_hit_link(t->page, cx, py);
}

// Re-runs layout for every tab that has a body, keeping each tab looking at
// the same part of its document. Called when the window is resized: the
// display list holds absolute positions computed for the old width, so
// without this a resize just clips or strands the text instead of reflowing
// it. Scroll is carried across as a fraction, because the new page is a
// different height and the old pixel offset means nothing in it.
static void relayout_all(browser_t* b, int view_h){
    for (int i = 0; i < MAX_TABS; i++){
        tab_t* t = &b->tabs[i];
        if (!t->used || !t->page || !t->src || !t->src_len) continue;
        if (t->state == BR_LOADING) continue;      // the fetch task owns resp

        int32_t old_h = t->page->height;
        int     old_s = t->scroll;

        // The tab label is the user's landmark, not a property of the
        // layout, so it survives a reflow even though html_layout would
        // happily set it again from the document.
        char keep[sizeof(t->title)];
        strncpy(keep, t->title, sizeof(keep) - 1);
        keep[sizeof(keep) - 1] = 0;

        layout_tab(b, t, t->src, t->src_len, t->src_plain, t->src_keep_chrome);

        strncpy(t->title, keep, sizeof(t->title) - 1);
        t->title[sizeof(t->title) - 1] = 0;

        // Carry the reading position across as a fraction: the reflowed page
        // is a different height, so the old pixel offset means nothing in it.
        if (t->page && old_h > 0){
            t->scroll = (int)(((int64_t)old_s * t->page->height) / old_h);
            int max = t->page->height - view_h;
            if (t->scroll > max) t->scroll = max;
            if (t->scroll < 0)   t->scroll = 0;
        }
    }
}

void app_browser_relayout(void){
    if (!active) return;
    relayout_all(active, active->view_h);
    wm_invalidate();
}

// Queues the current page's images that are not cached yet, and starts its
// video. Runs on the window's tick rather than once after layout, so images
// that could not be queued first time round (the queue was full) are picked
// up as it drains.
static void media_tick(browser_t* b){
    tab_t* t = cur_tab(b);
    if (t->used && t->page && t->state == BR_IDLE){
        for (uint32_t i = 0; i < t->page->box_n; i++){
            const html_box_t* bx = &t->page->boxes[i];
            if (bx->kind != HTML_BOX_FRAME || !bx->src_len) continue;
            if (b->img_head - b->img_tail >= IMG_Q) break;
            char abs[URL_MAX];
            if (box_abs_src(t, i, abs, sizeof(abs)) != 0) continue;
            if (image_cache_get(abs, 0) != IMG_NONE) continue;
            if (image_cache_claim(abs)) img_push(b, abs);
        }

        if (!t->video && !t->video_tried){
            for (uint32_t i = 0; i < t->page->box_n; i++){
                if (t->page->boxes[i].kind != HTML_BOX_VIDEO || !t->page->boxes[i].src_len) continue;
                char abs[URL_MAX];
                if (box_abs_src(t, i, abs, sizeof(abs)) != 0) continue;
                fetch_address(abs, t->video_src, sizeof(t->video_src));
                char* q = strchr(t->video_src, '?');
                if (q) *q = 0;
                t->video_tried = 1;
                t->video_box   = (int)i;
                video_start(b, t, 0);
                break;
            }
        }
    }

    if (t->video && t->seek_target >= 0 && ticks - t->seek_at >= SEEK_SETTLE)
        video_start(b, t, (uint32_t)t->seek_target);

    // Auto quality, the way YouTube's own Auto behaves in one direction:
    // a stream that cannot hold 20 pictures a second for three seconds in a
    // row -- after six seconds to settle, and never while buffering, which is
    // the network's problem and not the decoder's -- steps down a quality.
    // Picking a quality by hand turns this off.
    if (t->video && !t->video_prev && !b->quality_manual
        && video_state(t->video) == VIDEO_PLAYING && ticks - t->slow_mark >= 100){
        t->slow_mark = ticks;
        if (ticks - t->video_started >= 600 && video_fps(t->video) < 20) t->slow_secs++;
        else t->slow_secs = 0;
        if (t->slow_secs >= 3){
            int idx = 0;
            for (int i = 0; i < QUALITY_N; i++) if (QUALITY[i] == b->quality) idx = i;
            if (idx > 0){
                b->quality = QUALITY[idx - 1];
                video_start(b, t, video_where(t));
            }
            t->slow_secs = 0;
        }
    }

    // A replaced stream goes once its successor has something to show.
    if (t->video_prev && t->video){
        uint32_t serial;
        int st = video_state(t->video);
        if (video_picture(t->video, 0, &serial) || st == VIDEO_ERROR || st == VIDEO_ENDED){
            video_close(t->video_prev);
            t->video_prev = 0;
        }
    }
    if (b->fullscreen && !t->video) set_fullscreen(b, 0);

    // A video in a tab nobody is looking at pauses rather than playing to an
    // empty room -- and it would otherwise keep the sound device.
    for (int i = 0; i < MAX_TABS; i++){
        if (i == b->cur || !b->tabs[i].video) continue;
        if (video_state(b->tabs[i].video) == VIDEO_PLAYING) video_toggle_pause(b->tabs[i].video);
    }
}

// A YouTube address whose gateway did not answer. The bare network error is
// true but useless: what the user needs to know is that YouTube goes through
// a program on the host, and how to start it.
static void show_gateway_notice(browser_t* b, tab_t* t){
    ksnprintf(b->notice, sizeof(b->notice),
        "<h1>YouTube gateway not running</h1>"
        "<p>hawkOS reaches YouTube through a small gateway program on the host "
        "machine, <b>tools/ytgate.py</b>, which it expects at %s. Nothing answered "
        "there (%s).</p>"
        "<h2>To start it</h2>"
        "<p>Quit QEMU and start hawkOS with <b>make youtube</b> instead of make run. "
        "That starts the gateway, boots the machine, and stops the gateway again "
        "when you quit.</p>"
        "<p>Or leave this machine running and start the gateway by hand in another "
        "terminal: <b>python3 tools/ytgate.py</b> - then press Ctrl+R here.</p>"
        "<p>The gateway needs yt-dlp and ffmpeg on the host, in PATH or "
        "~/.local/bin.</p>",
        YT_GATEWAY, t->status);
    layout_tab(b, t, b->notice, strlen(b->notice), 0, 0);
    strcpy(t->title, "YouTube");
}

// A search page is mostly a box to type in: short on text, not empty.
static int has_text_field(const html_page_t* p){
    if (!p) return 0;
    for (uint32_t i = 0; i < p->field_n; i++)
        if (p->fields[i].kind == HTML_FIELD_TEXT) return 1;
    return 0;
}

static void on_tick(browser_t* b){
    media_tick(b);
    for (int i = 0; i < MAX_TABS; i++){
        tab_t* e = &b->tabs[i];
        char real[URL_MAX];
        if (e->used && e->state == BR_ERROR && !e->gateway_notice
            && yt_to_gateway(e->url, real, sizeof(real))){
            show_gateway_notice(b, e);
            e->gateway_notice = 1;    // shown; the red status line stays
            e->resp.final_url[0] = 0; // the notice is not a gateway page
            wm_invalidate();
        }
    }
    for (int i = 0; i < MAX_TABS; i++){
        tab_t* t = &b->tabs[i];
        if (!t->used || t->state != BR_READY || !t->resp.body) continue;

        int is_plain = t->resp.content_type[0]
                    && kstrnicmp(t->resp.content_type, "text/plain", 10) == 0;
        layout_tab(b, t, (const char*)t->resp.body, t->resp.body_len, is_plain, 0);

        gateway_to_yt(t->resp.final_url, t->url, URL_MAX);
        t->url_len = (int)strlen(t->url);

        if (t->pending_scroll > 0 && t->page){
            t->scroll = t->pending_scroll;
            int max = t->page->height - b->view_h;
            if (t->scroll > max) t->scroll = max > 0 ? max : 0;
        }
        t->pending_scroll = -1;

        // Two fallbacks, cheapest first. A page that laid out to almost
        // nothing gets a second pass with the reader-mode heuristics off: on
        // a site whose whole body sits inside a <nav>, or under a class name
        // this renderer treats as furniture, those heuristics are what
        // emptied it, and showing the menus beats showing a void.
        int empty = (!is_plain && html_text_len(t->page) < EMPTY_CHARS && !has_text_field(t->page));
        if (empty){
            layout_tab(b, t, (const char*)t->resp.body, t->resp.body_len, is_plain, 1);
            empty = (html_text_len(t->page) < EMPTY_CHARS && !has_text_field(t->page));
        }
        // Still nothing, and not simply a redirect page whose only job was
        // to carry a meta refresh -- those are supposed to be empty.
        if (empty && !(t->page && t->page->refresh[0]))
            show_empty_notice(b, t);

        if (!t->page || !t->page->title[0]){
            url_t u;
            if (url_parse(t->url, &u) == 0){
                strncpy(t->title, u.host, sizeof(t->title) - 1);
                t->title[sizeof(t->title) - 1] = 0;
            }
        }

        t->state = BR_IDLE;

        // Follow a meta refresh, but only forwards and only a few times:
        // some pages refresh to themselves to defeat caching.
        if (t->page && t->page->refresh[0] && t->refresh_hops < 3){
            char abs[URL_MAX];
            url_t base;
            if (url_parse(t->url, &base) == 0){
                url_resolve(&base, t->page->refresh, abs, sizeof(abs));
            } else {
                strncpy(abs, t->page->refresh, sizeof(abs) - 1);
                abs[sizeof(abs) - 1] = 0;
            }

            if (strcmp(abs, t->url) != 0 && i == b->cur){
                t->refresh_hops++;
                navigate(b, abs);
            }
        }

        wm_invalidate();
    }
}

static void handler(wm_window_t* win, const wm_event_t* ev){
    browser_t* b = (browser_t*)win->user;
    if (!b) return;

    switch (ev->type){
        case WM_EV_PAINT:      paint(win, b); break;
        case WM_EV_VIDEO:      paint_video_only(win, b); break;
        case WM_EV_TICK:       on_tick(b); break;
        case WM_EV_KEY:        on_key(b, ev->key); wm_invalidate(); break;
        case WM_EV_MOUSE_DOWN: on_click(win, b, ev->x, ev->y); wm_invalidate(); break;
        case WM_EV_MOUSE_UP:
            b->drag_scroll = 0;
            if (b->sel_drag){
                b->sel_drag = 0;
                int link = b->press_link;
                b->press_link = -1;
                if (!b->sel_moved && link >= 0){
                    b->sel_page = 0;
                    follow_link(b, cur_tab(b), link);
                }
                wm_invalidate();
            }
            break;

        case WM_EV_RESIZE:
            b->view_w = ev->x - SCROLLBAR_W;
            b->view_h = ev->y - TABBAR_H - TOOLBAR_H - STATUS_H;
            if (b->view_w < 200) b->view_w = 200;
            if (b->view_h < 60)  b->view_h = 60;
            relayout_all(b, b->view_h);
            wm_invalidate();
            break;

        case WM_EV_MOUSE_MOVE:
            if (b->sel_drag){
                tab_t* t = cur_tab(b);
                int dx = ev->x - b->press_x, dy = ev->y - b->press_y;
                if (b->sel_moved || dx * dx + dy * dy >= 16){
                    b->sel_moved = 1;
                    int vy = TABBAR_H + TOOLBAR_H;
                    // Past the top or bottom edge of the page the view scrolls,
                    // so a selection can be carried to text that is not yet
                    // on screen.
                    if (ev->y < vy) t->scroll -= 24;
                    else if (ev->y > vy + b->view_h) t->scroll += 24;
                    clamp_scroll(b);
                    b->sel_head = html_text_pos(t->page, ev->x, ev->y - vy + t->scroll);
                    wm_invalidate();
                }
            }
            if (b->drag_scroll){
                tab_t* t = cur_tab(b);
                int rel = ev->y - TABBAR_H - TOOLBAR_H;
                if (t->page && t->page->height > b->view_h)
                    t->scroll = rel * (t->page->height - b->view_h) / b->view_h;
                clamp_scroll(b);
                wm_invalidate();
            }
            break;

        case WM_EV_CLOSE:
            // The fetch task keeps running and would write into freed memory,
            // so the state it owns outlives the window rather than being
            // freed here. Only one browser window exists at a time, and
            // reopening reuses this same block.
            win->user = 0;
            b->win = 0;
            b->fullscreen = 0;
            b->fs_maximized = 0;
            // Nothing is left on screen to watch, so nothing should be left
            // playing. Reopening the window starts the page's video again.
            for (int i = 0; i < MAX_TABS; i++) stop_video(&b->tabs[i]);
            break;

        default: break;
    }
}

void app_browser_open(void){
    if (!active){
        active = (browser_t*)kmalloc(sizeof(browser_t));
        if (!active) return;
        memset(active, 0, sizeof(*active));
        active->fetch_tab = -1;
        active->quality = QUALITY_DEFAULT;
        active->volume  = 100;
        active->view_w = 780;
        active->view_h = 420;
        active->tabs[0].used = 1;
        active->cur = 0;
        show_home(active, &active->tabs[0]);
        task_create("browser-net", fetch_task, active);
    }

    wm_open("Browser", 150, 40, 900, 660, handler, active);
}
