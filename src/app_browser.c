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
#include "header/task.h"
#include "header/http.h"
#include "header/html.h"
#include "header/net.h"
#include "header/netcfg.h"
#include "header/font.h"

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
    int             history_n;

    // Bounded so a page that refreshes to itself cannot spin forever.
    int             refresh_hops;
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
    "<li><a href=\"https://en.wikipedia.org/wiki/Operating_system\">Wikipedia: Operating system</a></li>"
    "<li><a href=\"https://en.wikipedia.org/wiki/MS-DOS\">Wikipedia: MS-DOS</a></li>"
    "<li><a href=\"https://lite.duckduckgo.com/lite/?q=hobby+operating+system\">A search results page</a></li>"
    "<li><a href=\"https://example.com\">example.com over HTTPS</a></li>"
    "<li><a href=\"http://example.com\">example.com over plain HTTP</a></li>"
    "</ul>"
    "<h2>Using it</h2>"
    "<ul>"
    "<li>Type a URL in the address bar, or any words to search</li>"
    "<li>Ctrl+T opens a tab, Ctrl+W closes one, Ctrl+R reloads</li>"
    "<li>Up / Down / PageUp / PageDown scroll, Home jumps to the top</li>"
    "<li>Backspace goes back</li>"
    "</ul>"
    "<p>Certificates are not verified against a trust store - see the note at "
    "the top of src/tls.c. Traffic is encrypted, but this is not a browser to "
    "type a password into.</p>";

static tab_t* cur_tab(browser_t* b){ return &b->tabs[b->cur]; }

// ------------------------------------------------------------ page loading

static void progress(void* ctx, const char* stage){
    browser_t* b = (browser_t*)ctx;
    int t = b->fetch_tab;
    if (t < 0 || t >= MAX_TABS) return;
    strncpy(b->tabs[t].status, stage, sizeof(b->tabs[t].status) - 1);
    b->tabs[t].status[sizeof(b->tabs[t].status) - 1] = 0;
    wm_invalidate();
}

static void fetch_task(void* arg){
    browser_t* b = (browser_t*)arg;

    for (;;){
        if (!b->fetch_go){ task_sleep(50); continue; }

        int t = b->fetch_tab;
        b->fetch_go = 0;
        if (t < 0 || t >= MAX_TABS){ continue; }

        tab_t* tab = &b->tabs[t];
        http_response_free(&tab->resp);
        memset(&tab->resp, 0, sizeof(tab->resp));

        if (http_get(b->fetch_url, &tab->resp, progress, b) == 0){
            ksnprintf(tab->status, sizeof(tab->status), "%d  %u bytes  %s",
                      tab->resp.status, tab->resp.body_len,
                      tab->resp.content_type[0] ? tab->resp.content_type : "");
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

static void build_search_url(const char* query, char* out, uint32_t cap){
    uint32_t o = 0;
    const char* pre = SEARCH_PREFIX;
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

    if (t->history_n < HISTORY_MAX && t->url[0]){
        strncpy(t->history[t->history_n], t->url, URL_MAX - 1);
        t->history[t->history_n][URL_MAX - 1] = 0;
        t->history_n++;
    }

    strncpy(t->url, url, URL_MAX - 1);
    t->url[URL_MAX - 1] = 0;
    t->url_len = (int)strlen(t->url);

    strncpy(b->fetch_url, t->url, URL_MAX - 1);
    b->fetch_url[URL_MAX - 1] = 0;

    strcpy(t->status, "Starting");
    t->state    = BR_LOADING;
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
    } else {
        build_search_url(typed, target, sizeof(target));
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
    t->url[0] = 0;
    navigate(b, prev);
}

static void reload(browser_t* b){
    tab_t* t = cur_tab(b);
    if (!t->url[0]) return;
    char again[URL_MAX];
    strncpy(again, t->url, URL_MAX - 1);
    again[URL_MAX - 1] = 0;
    t->url[0] = 0;
    navigate(b, again);
}

// ---------------------------------------------------------------- tabs

static void tab_reset(tab_t* t){
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
    gfx_clip_set((uint32_t)x, (uint32_t)y, (uint32_t)(w - 150), STATUS_H);
    gfx_text((uint32_t)(x + 10), (uint32_t)(y + 4), t->status, col, GFX_TRANSPARENT);
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

static void paint(wm_window_t* win, browser_t* b){
    int x, y, w, h;
    wm_client_rect(win, &x, &y, &w, &h);

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
    } else if (t->page){
        html_paint(t->page, x, vy, b->view_w, b->view_h, t->scroll);
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

static void on_key(browser_t* b, int key){
    tab_t* t = cur_tab(b);

    if (kbd_ctrl_down()){
        if (key == 't' || key == 'T'){ new_tab(b); return; }
        if (key == 'w' || key == 'W'){ close_tab(b, b->cur); return; }
        if (key == 'r' || key == 'R'){ reload(b); return; }
        if (key == 'l' || key == 'L'){ b->url_focus = 1; b->url_selected = 1; return; }
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

    switch (key){
        case KEY_DOWN:  t->scroll += 48;             break;
        case KEY_UP:    t->scroll -= 48;             break;
        case KEY_PGDN:  t->scroll += b->view_h - 40; break;
        case KEY_PGUP:  t->scroll -= b->view_h - 40; break;
        case KEY_HOME:  t->scroll = 0;               break;
        case KEY_END:   t->scroll = t->page ? t->page->height : 0; break;
        case '\b':      go_back(b);                  return;
        case '\n':      b->url_focus = 1; b->url_selected = 1; return;
        default: break;
    }
    clamp_scroll(b);
}

static void on_click(wm_window_t* win, browser_t* b, int cx, int cy){
    int x, y, w, h;
    wm_client_rect(win, &x, &y, &w, &h);
    (void)x; (void)y;

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
    int idx = html_hit_link(t->page, cx, cy - vy + t->scroll);
    if (idx < 0) return;

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

static void on_tick(browser_t* b){
    for (int i = 0; i < MAX_TABS; i++){
        tab_t* t = &b->tabs[i];
        if (!t->used || t->state != BR_READY || !t->resp.body) continue;

        int is_plain = t->resp.content_type[0]
                    && kstrnicmp(t->resp.content_type, "text/plain", 10) == 0;
        layout_tab(b, t, (const char*)t->resp.body, t->resp.body_len, is_plain, 0);

        strncpy(t->url, t->resp.final_url, URL_MAX - 1);
        t->url[URL_MAX - 1] = 0;
        t->url_len = (int)strlen(t->url);

        // Two fallbacks, cheapest first. A page that laid out to almost
        // nothing gets a second pass with the reader-mode heuristics off: on
        // a site whose whole body sits inside a <nav>, or under a class name
        // this renderer treats as furniture, those heuristics are what
        // emptied it, and showing the menus beats showing a void.
        int empty = (!is_plain && html_text_len(t->page) < EMPTY_CHARS);
        if (empty){
            layout_tab(b, t, (const char*)t->resp.body, t->resp.body_len, is_plain, 1);
            empty = (html_text_len(t->page) < EMPTY_CHARS);
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
        case WM_EV_TICK:       on_tick(b); break;
        case WM_EV_KEY:        on_key(b, ev->key); wm_invalidate(); break;
        case WM_EV_MOUSE_DOWN: on_click(win, b, ev->x, ev->y); wm_invalidate(); break;
        case WM_EV_MOUSE_UP:   b->drag_scroll = 0; break;

        case WM_EV_RESIZE:
            b->view_w = ev->x - SCROLLBAR_W;
            b->view_h = ev->y - TABBAR_H - TOOLBAR_H - STATUS_H;
            if (b->view_w < 200) b->view_w = 200;
            if (b->view_h < 60)  b->view_h = 60;
            relayout_all(b, b->view_h);
            wm_invalidate();
            break;

        case WM_EV_MOUSE_MOVE:
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
        active->view_w = 780;
        active->view_h = 420;
        active->tabs[0].used = 1;
        active->cur = 0;
        show_home(active, &active->tabs[0]);
        task_create("browser-net", fetch_task, active);
    }

    wm_open("Browser", 150, 40, 900, 660, handler, active);
}
