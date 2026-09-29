#pragma once
#include <stdint.h>

// Intel ICH AC'97 audio -- QEMU's `-device AC97`, and the controller on a
// great many real boards of the early 2000s.
//
// Output only: 16-bit signed little-endian stereo PCM, pushed by the caller.
// The controller plays out of a ring of 32 buffer descriptors by DMA; this
// driver fills descriptors ahead of the play position and never touches one
// the hardware still owns, so playback survives the writer being descheduled
// for as long as the ring lasts (about a second and a half).

int      ac97_init(void);          // 0 if a codec was found and brought up
int      ac97_present(void);

// Asks for a sample rate. Returns the rate actually in effect: 48000 on a
// codec without variable-rate support, whatever was asked for otherwise.
uint32_t ac97_set_rate(uint32_t hz);
uint32_t ac97_rate(void);

// 0-100, applied to the master output.
void     ac97_set_volume(int percent);

// Queues `frames` stereo frames (2 samples each). Blocks, sleeping, while the
// ring is full. Returns the number of frames queued, which is `frames` unless
// the device is absent.
uint32_t ac97_write(const int16_t* samples, uint32_t frames);

// Frames queued and not yet played. The difference between what a player has
// written and this is where the listener actually is -- the clock to sync
// video against.
uint32_t ac97_pending(void);

// Holds playback where it is (on) or resumes it (off), keeping the queue.
void     ac97_pause(int on);

// Stops playback and discards everything queued.
void     ac97_stop(void);
