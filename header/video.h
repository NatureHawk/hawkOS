#pragma once
#include <stdint.h>

// Streaming MPEG-1 playback.
//
// video_open() starts two tasks and returns at once. One pulls the stream off
// the network into a large ring buffer (the download runs ahead of playback
// by as much as the ring holds, so a stall on the wire is absorbed rather
// than heard); the other feeds that to PL_MPEG, converts each decoded picture
// to the framebuffer's pixel format, and pushes the sound to the AC'97.
//
// Whoever displays the video polls video_frame() when repainting and calls
// video_close() when done with it. The handle stays valid until then even if
// the stream has ended or failed; memory is released once both tasks have
// noticed the close.

typedef struct video video_t;

typedef enum {
    VIDEO_CONNECTING = 0,
    VIDEO_BUFFERING,
    VIDEO_PLAYING,
    VIDEO_PAUSED,
    VIDEO_ENDED,
    VIDEO_ERROR
} video_state_t;

// `start_ms` is where in the video this stream was asked to begin; the
// server's X-Start header refines it once the response arrives.
video_t* video_open(const char* url, uint32_t start_ms);
void     video_close(video_t* v);

int         video_state(const video_t* v);
const char* video_status(const video_t* v);     // human-readable, for a status line

// The most recent decoded picture, as YUV 4:2:0 planes -- converted to RGB
// only as it is drawn (gfx_blit_yuv). Returns 0 before the first picture.
// `serial` changes every time a new picture replaces it.
typedef struct {
    const uint8_t *y, *cb, *cr;
    int w, h;               // picture size
    int ystride, cstride;   // bytes per row of the luma and chroma planes
} video_picture_t;

int video_picture(const video_t* v, video_picture_t* out, uint32_t* serial);

void     video_toggle_pause(video_t* v);
void     video_set_paused(video_t* v, int on);
int      video_is_paused(const video_t* v);
// Position in the whole video (not just this stream), and the video's length
// if the server said -- 0 when it did not.
uint32_t video_position_ms(const video_t* v);
uint32_t video_duration_ms(const video_t* v);
uint32_t video_buffered_kb(const video_t* v);  // downloaded, not yet decoded

// Decoder throughput, for the status line: pictures per second over the
// last second of playback.
uint32_t video_fps(const video_t* v);
