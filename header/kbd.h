#pragma once
#include <stdint.h>

// Special keys are returned above the ASCII range so a caller can switch on
// the result of kbd_getc() without a separate "is this a control key" flag.
#define KEY_UP      0x101
#define KEY_DOWN    0x102
#define KEY_LEFT    0x103
#define KEY_RIGHT   0x104
#define KEY_PGUP    0x105
#define KEY_PGDN    0x106
#define KEY_HOME    0x107
#define KEY_END     0x108
#define KEY_DELETE  0x109
#define KEY_F1      0x110

void kbd_init(void);

// Next key, or -1 if the buffer is empty. Never blocks.
int kbd_getc(void);

int kbd_ctrl_down(void);
int kbd_shift_down(void);
