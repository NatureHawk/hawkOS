// src/image.c — image decoding (through stb_image) and the image cache
#include <stdint.h>
#include "header/image.h"
#include "header/http.h"
#include "header/kheap.h"
#include "header/kstring.h"
#include "header/irqctl.h"

// stb_image's entry points, declared here rather than by including the
// header a second time: lib_stbimage.c is the one place it is compiled.
extern unsigned char* stbi_load_from_memory(const unsigned char* buffer, int len,
                                            int* x, int* y, int* comp, int req_comp);
extern void stbi_image_free(void* retval_from_stbi_load);

// Anything decoding larger than this many pixels is refused before a byte of
// output is allocated. 4096x4096 is past any image a page means to show at
// this resolution, and the RGBA intermediate alone would be 64 MB.
#define MAX_PIXELS (4096u * 4096u)

static int header_dims(const uint8_t* d, uint32_t n, uint32_t* w, uint32_t* h){
    // PNG: IHDR is always first.
    if (n >= 24 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G'){
        *w = ((uint32_t)d[16] << 24) | ((uint32_t)d[17] << 16) | ((uint32_t)d[18] << 8) | d[19];
        *h = ((uint32_t)d[20] << 24) | ((uint32_t)d[21] << 16) | ((uint32_t)d[22] << 8) | d[23];
        return 0;
    }
    // GIF: logical screen size.
    if (n >= 10 && d[0] == 'G' && d[1] == 'I' && d[2] == 'F'){
        *w = d[6] | ((uint32_t)d[7] << 8);
        *h = d[8] | ((uint32_t)d[9] << 8);
        return 0;
    }
    // JPEG: walk the markers to the first start-of-frame.
    if (n >= 4 && d[0] == 0xFF && d[1] == 0xD8){
        uint32_t i = 2;
        while (i + 9 < n){
            if (d[i] != 0xFF){ i++; continue; }
            uint8_t m = d[i + 1];
            if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC){
                *h = ((uint32_t)d[i + 5] << 8) | d[i + 6];
                *w = ((uint32_t)d[i + 7] << 8) | d[i + 8];
                return 0;
            }
            uint32_t seg = ((uint32_t)d[i + 2] << 8) | d[i + 3];
            i += 2 + seg;
        }
    }
    return -1;
}

image_t* image_decode(const uint8_t* data, uint32_t len, int max_w, uint32_t bg){
    if (!data || len < 8) return 0;

    uint32_t hw, hh;
    if (header_dims(data, len, &hw, &hh) == 0 && (uint64_t)hw * hh > MAX_PIXELS) return 0;

    int w = 0, h = 0, comp = 0;
    unsigned char* rgba = stbi_load_from_memory(data, (int)len, &w, &h, &comp, 4);
    if (!rgba || w <= 0 || h <= 0){
        if (rgba) stbi_image_free(rgba);
        return 0;
    }

    int ow = w, oh = h;
    if (max_w > 0 && w > max_w){
        ow = max_w;
        oh = (int)(((int64_t)h * max_w) / w);
        if (oh < 1) oh = 1;
    }

    image_t* img = (image_t*)kmalloc(sizeof(image_t));
    uint32_t* px = (uint32_t*)kmalloc((uint32_t)ow * (uint32_t)oh * 4u);
    if (!img || !px){
        if (img) kfree(img);
        if (px) kfree(px);
        stbi_image_free(rgba);
        return 0;
    }

    uint32_t br = (bg >> 16) & 0xFF, bgc = (bg >> 8) & 0xFF, bb = bg & 0xFF;
    uint32_t xstep = ((uint32_t)w << 16) / (uint32_t)ow;
    uint32_t ystep = ((uint32_t)h << 16) / (uint32_t)oh;

    for (int y = 0; y < oh; y++){
        uint32_t sy = ((uint32_t)y * ystep) >> 16;
        const unsigned char* row = rgba + (uint32_t)sy * (uint32_t)w * 4u;
        uint32_t* out = px + (uint32_t)y * (uint32_t)ow;
        for (int x = 0; x < ow; x++){
            uint32_t sx = ((uint32_t)x * xstep) >> 16;
            const unsigned char* s = row + sx * 4u;
            uint32_t a = s[3];
            uint32_t r = s[0], g = s[1], b = s[2];
            if (a != 255){
                r = (r * a + br  * (255 - a)) / 255;
                g = (g * a + bgc * (255 - a)) / 255;
                b = (b * a + bb  * (255 - a)) / 255;
            }
            out[x] = (r << 16) | (g << 8) | b;
        }
    }
    stbi_image_free(rgba);

    img->w  = ow;
    img->h  = oh;
    img->px = px;
    return img;
}

void image_free(image_t* img){
    if (!img) return;
    if (img->px) kfree(img->px);
    kfree(img);
}

// ------------------------------------------------------------------ cache
//
// A fixed table searched linearly: a page has tens of images, not
// thousands, and a string compare per entry per lookup is nothing next to
// painting them. Eviction is least-recently-used by a use counter, and only
// ever of READY or FAILED entries -- a PENDING one has a fetch in flight that
// will come back to it.

#define CACHE_N      160
#define CACHE_BUDGET (24u * 1024u * 1024u)

typedef struct {
    int      state;             // IMG_*; IMG_NONE marks a free slot
    char     url[URL_MAX];
    image_t* img;
    uint32_t bytes;
    uint32_t used_at;
} entry_t;

static entry_t  cache[CACHE_N];
static uint32_t clock = 1;
static uint32_t held_bytes = 0;

static entry_t* find(const char* url){
    for (int i = 0; i < CACHE_N; i++)
        if (cache[i].state != IMG_NONE && strcmp(cache[i].url, url) == 0) return &cache[i];
    return 0;
}

static void drop(entry_t* e){
    if (e->img) image_free(e->img);
    held_bytes -= e->bytes;
    memset(e, 0, sizeof(*e));
}

// Frees least-recently-used entries until `need` more bytes fit the budget,
// and returns a free slot (evicting one if the table is full).
static entry_t* make_room(uint32_t need){
    for (;;){
        entry_t* victim = 0;
        entry_t* free_slot = 0;
        for (int i = 0; i < CACHE_N; i++){
            entry_t* e = &cache[i];
            if (e->state == IMG_NONE){ if (!free_slot) free_slot = e; continue; }
            if (e->state == IMG_PENDING) continue;
            if (!victim || e->used_at < victim->used_at) victim = e;
        }
        if (free_slot && held_bytes + need <= CACHE_BUDGET) return free_slot;
        if (!victim) return free_slot;
        drop(victim);
    }
}

int image_cache_get(const char* url, const image_t** out){
    uint32_t f = irq_save();
    entry_t* e = find(url);
    int st = e ? e->state : IMG_NONE;
    if (e){
        e->used_at = clock++;
        if (out) *out = (st == IMG_READY) ? e->img : 0;
    } else if (out) *out = 0;
    irq_restore(f);
    return st;
}

int image_cache_claim(const char* url){
    uint32_t f = irq_save();
    int claimed = 0;
    if (!find(url)){
        entry_t* e = make_room(0);
        if (e){
            memset(e, 0, sizeof(*e));
            e->state = IMG_PENDING;
            strncpy(e->url, url, URL_MAX - 1);
            e->used_at = clock++;
            claimed = 1;
        }
    }
    irq_restore(f);
    return claimed;
}

void image_cache_put(const char* url, image_t* img){
    uint32_t bytes = img ? (uint32_t)img->w * (uint32_t)img->h * 4u : 0;
    uint32_t f = irq_save();
    entry_t* e = find(url);
    if (!e){
        e = make_room(bytes);
        if (!e){ irq_restore(f); image_free(img); return; }
        memset(e, 0, sizeof(*e));
        strncpy(e->url, url, URL_MAX - 1);
    } else if (bytes && held_bytes + bytes > CACHE_BUDGET){
        // Evict around this entry, which is PENDING and so never a victim.
        make_room(bytes);
    }
    e->state   = img ? IMG_READY : IMG_FAILED;
    e->img     = img;
    e->bytes   = bytes;
    e->used_at = clock++;
    held_bytes += bytes;
    irq_restore(f);
}

uint32_t image_cache_bytes(void){ return held_bytes; }

uint32_t image_cache_count(void){
    uint32_t n = 0;
    for (int i = 0; i < CACHE_N; i++) if (cache[i].state == IMG_READY) n++;
    return n;
}
