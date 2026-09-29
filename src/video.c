// src/video.c — streaming MPEG-1 player engine
//
// Network task -> ring buffer -> decoder task -> {frame buffers, AC'97}.
//
// The ring is where the spare memory goes: 8 MB is a minute or more of a
// 360p stream, so once playback starts the download is almost never the
// thing the viewer waits on. The two tasks share it through two monotonic
// byte counters, `head` (written) and `tail` (consumed), each written by one
// side only -- a single-producer single-consumer queue needs no lock.
//
// Timing is driven by the wall clock. Every pass of the decode loop hands
// PL_MPEG the time elapsed since the last one, and it decodes whatever
// pictures and sound fall due in that interval. Sound is decoded a fixed lead
// ahead of the pictures, which is the time it then spends queued in the
// AC'97 ring before it is heard -- so what is on screen and what is coming
// out of the speaker line up.
#include <stdint.h>
#include "header/video.h"
#include "header/http.h"
#include "header/task.h"
#include "header/sync.h"
#include "header/kheap.h"
#include "header/kstring.h"
#include "header/kprintf.h"
#include "header/irqctl.h"
#include "header/ac97.h"
#include "header/wm.h"
#define PLM_NO_STDIO
#include "third_party/pl_mpeg/pl_mpeg.h"

extern volatile unsigned long long ticks;

#define RING_CAP     (8u * 1024u * 1024u)
// Bytes held before the first picture: about three seconds at 480p. Less, and
// the download -- still ramping up while the gateway starts its transcode --
// could fall behind playback a few seconds in and leave a gap in the sound.
#define PREBUFFER    (768u * 1024u)
#define FEED_CHUNK   (64u * 1024u)        // largest single hand-off to PL_MPEG
// Seconds of sound decoded ahead of the picture, which is also how long a
// stall elsewhere (a page layout, a burst of image decoding on the browser's
// task) can last before the sound card runs dry. The AC'97 ring holds 1.5 s,
// so this leaves it room. Pausing halts the DMA, so a deep queue does not
// mean sound carrying on after the picture stops.
#define AUDIO_LEAD   0.80
#define MAX_STEP     0.25                 // most wall-clock time one pass may cover

struct video {
    char              url[URL_MAX];
    volatile int      state;
    char              status[96];

    uint8_t*          ring;
    volatile uint32_t head, tail;
    volatile int      net_eof;
    waitq_t           ring_wq;         // woken when the ring gains data or space, and on eof/close

    volatile int      closed;          // owner is done with the handle
    volatile int      tasks;           // tasks still running

    // Two pictures, each a YUV 4:2:0 copy of what the decoder produced: the
    // one on screen (front) and the one being written. 1.5 bytes a pixel,
    // against 4 for the RGB they used to be converted to up front.
    uint8_t*          frames[2];
    int               fw, fh;
    int               ystride, cstride, ch;
    plm_frame_t*      latest;           // newest picture of the current decode pass
    volatile int      front;
    volatile uint32_t serial;

    volatile int      paused;
    volatile uint32_t pos_ms;

    // Where in the video this stream begins, and how long the whole video
    // is, as the server reported them (X-Start, X-Duration). A stream opened
    // part-way through counts its pictures from zero, so the start is what
    // turns a decoder's clock into a position in the video.
    uint32_t          start_ms;
    uint32_t          duration_ms;

    uint32_t          fps;
    uint32_t          fps_count;
    unsigned long long fps_mark;

    // Sound: output rate, and the fractional read position a resampler needs
    // when the codec cannot run at the stream's own rate.
    uint32_t          in_rate, out_rate;
    uint32_t          rs_frac;         // 16.16 position within the input frame
    int16_t*          pcm;             // conversion scratch
};

// Only one stream plays sound at a time; the newest to start takes the
// device, and an older one carries on silently.
static video_t* audio_owner = 0;

static void set_status(video_t* v, int state, const char* msg){
    v->state = state;
    strncpy(v->status, msg, sizeof(v->status) - 1);
    v->status[sizeof(v->status) - 1] = 0;
    wm_invalidate();
}

static void free_all(video_t* v){
    for (int i = 0; i < 2; i++) if (v->frames[i]) kfree(v->frames[i]);
    if (v->pcm)  kfree(v->pcm);
    if (v->ring) kfree(v->ring);
    kfree(v);
}

// Called by each task on its way out. The last one out frees the handle if
// the owner has already let go of it; otherwise video_close does.
static void task_leave(video_t* v){
    uint32_t f = irq_save();
    int last = (--v->tasks == 0) && v->closed;
    irq_restore(f);
    if (last) free_all(v);
}

// ------------------------------------------------------------- network

// "299.633" -> 299633. Header values only; no signs, no exponents.
static uint32_t parse_ms(const char* s){
    uint32_t whole = 0, frac = 0, scale = 1000;
    while (*s == ' ') s++;
    while (*s >= '0' && *s <= '9') whole = whole * 10 + (uint32_t)(*s++ - '0');
    if (*s == '.'){
        s++;
        while (*s >= '0' && *s <= '9' && scale > 1){
            scale /= 10;
            frac += (uint32_t)(*s++ - '0') * scale;
        }
    }
    return whole * 1000 + frac;
}

static void net_progress(void* ctx, const char* stage){
    video_t* v = (video_t*)ctx;
    if (v->state == VIDEO_CONNECTING) set_status(v, VIDEO_CONNECTING, stage);
}

static void net_task(void* arg){
    video_t* v = (video_t*)arg;
    char err[96];

    http_stream_t* s = http_open(v->url, net_progress, v, err, sizeof(err));
    if (!s){
        char m[96];
        ksnprintf(m, sizeof(m), "Could not load video: %s", err);
        set_status(v, VIDEO_ERROR, m);
        v->net_eof = 1;
        waitq_wake_all(&v->ring_wq);
        task_leave(v);
        task_exit();
    }
    if (http_status(s) != 200){
        char m[96];
        ksnprintf(m, sizeof(m), "Video server answered %d", http_status(s));
        set_status(v, VIDEO_ERROR, m);
        http_close(s);
        v->net_eof = 1;
        waitq_wake_all(&v->ring_wq);
        task_leave(v);
        task_exit();
    }

    char hv[32];
    if (http_header(s, "X-Start", hv, sizeof(hv)) == 0)    v->start_ms = parse_ms(hv);
    if (http_header(s, "X-Duration", hv, sizeof(hv)) == 0) v->duration_ms = parse_ms(hv);
    v->pos_ms = v->start_ms;

    if (v->state == VIDEO_CONNECTING) set_status(v, VIDEO_BUFFERING, "Buffering");

    unsigned long long last_data = ticks;
    while (!v->closed){
        uint32_t seq   = waitq_seq(&v->ring_wq);      // before looking, so a wake in between counts
        uint32_t used  = v->head - v->tail;
        uint32_t space = RING_CAP - used;
        if (space < 16384){ waitq_wait_seq(&v->ring_wq, seq, 20); last_data = ticks; continue; }

        uint32_t at   = v->head % RING_CAP;
        uint32_t room = RING_CAP - at;
        if (room > space) room = space;
        if (room > FEED_CHUNK) room = FEED_CHUNK;

        int n = http_read(s, v->ring + at, room);
        if (n > 0){
            v->head += (uint32_t)n;
            waitq_wake_all(&v->ring_wq);
            last_data = ticks;
        } else if (n == 0){
            // A transcoding proxy can go quiet for a while between bursts;
            // a minute of silence is a dead connection, not a slow one.
            if (ticks - last_data > 6000){
                if (v->state != VIDEO_ENDED) set_status(v, VIDEO_ERROR, "Video stream stalled");
                break;
            }
            http_wait(s, 20);
        } else {
            break;
        }
    }

    http_close(s);
    v->net_eof = 1;
    waitq_wake_all(&v->ring_wq);
    task_leave(v);
    task_exit();
}

// ------------------------------------------------------------- decoding

// PL_MPEG asks for more bytes whenever its buffer runs dry. Blocking here is
// what turns a network stall into a pause instead of a corrupt picture: the
// decoder simply does not return until the bytes it needs exist.
static void load_cb(plm_buffer_t* buf, void* user){
    video_t* v = (video_t*)user;

    int waited = 0;
    for (;;){
        uint32_t seq = waitq_seq(&v->ring_wq);
        if (v->head != v->tail) break;
        if (v->net_eof || v->closed){
            plm_buffer_signal_end(buf);
            return;
        }
        if (!waited && v->state == VIDEO_PLAYING) set_status(v, VIDEO_BUFFERING, "Buffering");
        waited = 1;
        waitq_wait_seq(&v->ring_wq, seq, 20);
    }
    if (waited && v->state == VIDEO_BUFFERING) set_status(v, VIDEO_PLAYING, "Playing");

    uint32_t avail = v->head - v->tail;
    if (avail > FEED_CHUNK) avail = FEED_CHUNK;
    uint32_t at = v->tail % RING_CAP;
    uint32_t first = RING_CAP - at;
    if (first > avail) first = avail;

    plm_buffer_write(buf, v->ring + at, first);
    if (avail > first) plm_buffer_write(buf, v->ring, avail - first);
    v->tail += avail;
    waitq_wake_all(&v->ring_wq);            // the downloader may have been waiting for room
}

// PL_MPEG reports every picture it decodes, and when playback is running
// behind that can be several per pass. Only the last of them will ever be
// seen, so the callback just notes it and publish() copies that one out once
// the pass is over -- the decoder's frame stays valid until the next pass.
static void on_video(plm_t* plm, plm_frame_t* frame, void* user){
    (void)plm;
    video_t* v = (video_t*)user;
    if ((int)frame->width != v->fw || (int)frame->height != v->fh) return;
    v->latest = frame;
}

static void publish(video_t* v){
    plm_frame_t* frame = v->latest;
    if (!frame) return;
    v->latest = 0;

    int back = v->front ^ 1;
    uint8_t* dst = v->frames[back];
    uint32_t ysize = (uint32_t)v->ystride * (uint32_t)frame->y.height;
    uint32_t csize = (uint32_t)v->cstride * (uint32_t)v->ch;
    memcpy(dst, frame->y.data, ysize);
    memcpy(dst + ysize, frame->cb.data, csize);
    memcpy(dst + ysize + csize, frame->cr.data, csize);
    v->front = back;
    v->serial++;
    v->pos_ms = v->start_ms + (uint32_t)(frame->time * 1000.0);

    v->fps_count++;
    if (ticks - v->fps_mark >= 100){
        v->fps       = v->fps_count;
        v->fps_count = 0;
        v->fps_mark  = ticks;
    }
    wm_invalidate_video();
}

static inline int16_t to_s16(float f){
    if (f >  1.0f) f =  1.0f;
    if (f < -1.0f) f = -1.0f;
    return (int16_t)(f * 32767.0f);
}

static void on_audio(plm_t* plm, plm_samples_t* samples, void* user){
    (void)plm;
    video_t* v = (video_t*)user;
    if (audio_owner != v || !ac97_present() || v->closed) return;

    uint32_t n = samples->count;
    const float* in = samples->interleaved;

    if (v->out_rate == v->in_rate){
        for (uint32_t i = 0; i < n * 2; i++) v->pcm[i] = to_s16(in[i]);
        ac97_write(v->pcm, n);
        return;
    }

    // Nearest-sample rate conversion. Crude, but only reached on a codec
    // stuck at 48 kHz, and it keeps the audio clock honest either way.
    uint32_t step = (uint32_t)(((uint64_t)v->in_rate << 16) / v->out_rate);
    uint32_t out = 0;
    while ((v->rs_frac >> 16) < n && out < PLM_AUDIO_SAMPLES_PER_FRAME * 2){
        uint32_t i = v->rs_frac >> 16;
        v->pcm[out * 2]     = to_s16(in[i * 2]);
        v->pcm[out * 2 + 1] = to_s16(in[i * 2 + 1]);
        out++;
        v->rs_frac += step;
    }
    v->rs_frac -= n << 16;
    ac97_write(v->pcm, out);
}

static void dec_task(void* arg){
    video_t* v = (video_t*)arg;
    plm_t* plm = 0;

    // Hold back until there is enough to decode a stretch without stalling.
    uint32_t pseq;
    while ((pseq = waitq_seq(&v->ring_wq), !v->closed && !v->net_eof && (v->head - v->tail) < PREBUFFER)){
        if (v->state == VIDEO_BUFFERING){
            char m[96];
            ksnprintf(m, sizeof(m), "Buffering %u KB", (v->head - v->tail) / 1024u);
            strncpy(v->status, m, sizeof(v->status) - 1);
            wm_invalidate();
        }
        waitq_wait_seq(&v->ring_wq, pseq, 50);
    }
    if (v->closed || v->state == VIDEO_ERROR) goto out;
    if (v->head == v->tail){ set_status(v, VIDEO_ERROR, "The video stream was empty"); goto out; }

    plm_buffer_t* buf = plm_buffer_create_with_capacity(512u * 1024u);
    if (!buf){ set_status(v, VIDEO_ERROR, "Out of memory for the decoder"); goto out; }
    plm_buffer_set_load_callback(buf, load_cb, v);

    plm = plm_create_with_buffer(buf, 1);
    if (!plm){ plm_buffer_destroy(buf); set_status(v, VIDEO_ERROR, "Out of memory for the decoder"); goto out; }

    if (!plm_has_headers(plm) || plm_get_width(plm) <= 0){
        set_status(v, VIDEO_ERROR, "Not an MPEG-1 stream");
        goto out;
    }

    v->fw = plm_get_width(plm);
    v->fh = plm_get_height(plm);
    // Plane geometry as the decoder lays it out: rounded up to whole 16x16
    // macroblocks, with the chroma planes half that each way.
    v->ystride = ((v->fw + 15) / 16) * 16;
    v->cstride = v->ystride / 2;
    v->ch      = ((v->fh + 15) / 16) * 8;
    uint32_t fbytes = (uint32_t)v->ystride * (uint32_t)(v->ch * 2)
                    + 2u * (uint32_t)v->cstride * (uint32_t)v->ch;
    for (int i = 0; i < 2; i++){
        v->frames[i] = (uint8_t*)kmalloc(fbytes);
        if (!v->frames[i]){ set_status(v, VIDEO_ERROR, "Out of memory for frames"); goto out; }
        memset(v->frames[i], 0, fbytes);
    }

    plm_set_loop(plm, 0);
    plm_set_video_decode_callback(plm, on_video, v);

    int with_audio = ac97_present() && plm_get_num_audio_streams(plm) > 0;
    if (with_audio){
        v->pcm = (int16_t*)kmalloc(PLM_AUDIO_SAMPLES_PER_FRAME * 2 * sizeof(int16_t) * 2);
        if (!v->pcm) with_audio = 0;
    }
    if (with_audio){
        if (audio_owner && audio_owner != v) ac97_stop();
        audio_owner = v;
        v->in_rate  = (uint32_t)plm_get_samplerate(plm);
        v->out_rate = ac97_set_rate(v->in_rate);
        plm_set_audio_enabled(plm, 1);
        plm_set_audio_stream(plm, 0);
        plm_set_audio_decode_callback(plm, on_audio, v);
        plm_set_audio_lead_time(plm, AUDIO_LEAD);
    } else {
        plm_set_audio_enabled(plm, 0);
    }

    kprintf("[video] %dx%d @ %u fps, audio %s (%u Hz -> %u Hz)\n",
            v->fw, v->fh, (uint32_t)plm_get_framerate(plm),
            with_audio ? "on" : "off", v->in_rate, v->out_rate);
    set_status(v, VIDEO_PLAYING, "Playing");
    v->fps_mark = ticks;

    unsigned long long last = ticks;
    while (!v->closed){
        if (v->paused){
            if (v->state != VIDEO_PAUSED){
                set_status(v, VIDEO_PAUSED, "Paused");
                if (audio_owner == v) ac97_pause(1);
            }
            task_sleep(20);
            last = ticks;
            continue;
        }
        if (v->state == VIDEO_PAUSED){
            if (audio_owner == v) ac97_pause(0);
            set_status(v, VIDEO_PLAYING, "Playing");
        }

        unsigned long long now = ticks;
        double dt = (double)(now - last) / 100.0;
        last = now;
        if (dt > MAX_STEP) dt = MAX_STEP;
        if (dt > 0) plm_decode(plm, dt);
        publish(v);

        if (plm_has_ended(plm)){
            set_status(v, VIDEO_ENDED, "Ended");
            break;
        }
        task_sleep(10);
    }

out:
    if (audio_owner == v){
        // Let what is already queued finish unless the viewer walked away.
        if (v->closed) ac97_stop();
        audio_owner = 0;
    }
    if (plm) plm_destroy(plm);
    // Frames stay allocated while the handle is open so the last picture
    // remains on screen after the end; whoever frees the handle frees them.
    task_leave(v);
    task_exit();
}

// ------------------------------------------------------------------ API

video_t* video_open(const char* url, uint32_t start_ms){
    video_t* v = (video_t*)kmalloc(sizeof(video_t));
    if (!v) return 0;
    memset(v, 0, sizeof(*v));
    strncpy(v->url, url, sizeof(v->url) - 1);

    v->ring = (uint8_t*)kmalloc(RING_CAP);
    if (!v->ring){ kfree(v); return 0; }

    // Until the server says where the stream really begins, it begins where
    // it was asked to: a position of zero here is what sent a second quick
    // seek back to the start of the video.
    v->start_ms = start_ms;
    v->pos_ms   = start_ms;
    v->state = VIDEO_CONNECTING;
    strcpy(v->status, "Connecting");
    v->tasks = 2;

    if (task_create("video-net", net_task, v) < 0){
        kfree(v->ring); kfree(v);
        return 0;
    }
    if (task_create("video-dec", dec_task, v) < 0){
        // The network task is already running and will leave on its own
        // once it sees the close; account for the task that never started.
        v->tasks = 1;
        set_status(v, VIDEO_ERROR, "No free task slot for the decoder");
    }
    return v;
}

void video_close(video_t* v){
    if (!v) return;
    uint32_t f = irq_save();
    v->closed = 1;
    int none_left = (v->tasks == 0);
    irq_restore(f);
    if (!none_left) waitq_wake_all(&v->ring_wq);

    if (none_left) free_all(v);
}

int video_state(const video_t* v){ return v ? v->state : VIDEO_ERROR; }
const char* video_status(const video_t* v){ return v ? v->status : ""; }

int video_picture(const video_t* v, video_picture_t* out, uint32_t* serial){
    if (!v || !v->serial){ if (serial) *serial = 0; return 0; }
    if (out){
        const uint8_t* base = v->frames[v->front];
        uint32_t ysize = (uint32_t)v->ystride * (uint32_t)(v->ch * 2);
        uint32_t csize = (uint32_t)v->cstride * (uint32_t)v->ch;
        out->y  = base;
        out->cb = base + ysize;
        out->cr = base + ysize + csize;
        out->w = v->fw;
        out->h = v->fh;
        out->ystride = v->ystride;
        out->cstride = v->cstride;
    }
    if (serial) *serial = v->serial;
    return 1;
}

void video_toggle_pause(video_t* v){
    if (!v) return;
    if (v->state != VIDEO_PLAYING && v->state != VIDEO_PAUSED && v->state != VIDEO_BUFFERING) return;
    v->paused = !v->paused;
    wm_invalidate();
}

void video_set_paused(video_t* v, int on){
    if (!v) return;
    v->paused = on ? 1 : 0;
    wm_invalidate();
}

int video_is_paused(const video_t* v){ return v ? v->paused : 0; }

uint32_t video_position_ms(const video_t* v){ return v ? v->pos_ms : 0; }
uint32_t video_duration_ms(const video_t* v){ return v ? v->duration_ms : 0; }
uint32_t video_buffered_kb(const video_t* v){ return v ? (v->head - v->tail) / 1024u : 0; }
uint32_t video_fps(const video_t* v){ return v ? v->fps : 0; }
