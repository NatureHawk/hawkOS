#pragma once
#include <stdint.h>

// Decoded web images, and the cache that keeps them.
//
// Pictures are stored ready to blit: 0x00RRGGBB, already composited over the
// page background where the source had transparency, and already scaled down
// if the original was far larger than anything the browser can show. That
// keeps both painting and memory predictable -- a 4000-pixel photo becomes
// the 720-pixel image it will be drawn as, not 64 MB of pixels.

typedef struct {
    int       w, h;
    uint32_t* px;
} image_t;

// Decodes JPEG, PNG, GIF (first frame) or BMP. Returns 0 on anything it
// cannot read. `max_w` bounds the stored width (0 for no bound).
image_t* image_decode(const uint8_t* data, uint32_t len, int max_w, uint32_t bg);
void     image_free(image_t* img);

// ------------------------------------------------------------------ cache

enum {
    IMG_NONE = 0,       // never asked for
    IMG_PENDING,        // queued or being fetched
    IMG_READY,
    IMG_FAILED
};

// State of `url` in the cache, and the image when READY. Marks the entry as
// recently used, so what is on screen is the last thing evicted.
int      image_cache_get(const char* url, const image_t** out);

// Claims `url` for fetching: returns 1 if the caller should fetch it (the
// entry is now PENDING), 0 if it is already pending, ready or failed.
int      image_cache_claim(const char* url);

// Stores a fetch result. `img` becomes the cache's; pass 0 for a failure.
void     image_cache_put(const char* url, image_t* img);

// Bytes of pixels held, and entries in use.
uint32_t image_cache_bytes(void);
uint32_t image_cache_count(void);
