#pragma once
#include <stdint.h>

void mouse_init(void);        // programs the 8042 aux port, enables IRQ12
void mouse_handler_c(void);   // called from irq12_stub

int32_t mouse_x(void);
int32_t mouse_y(void);
int     mouse_left_button(void);

// Accumulated scroll-wheel notches since the last call, then reset.
// Positive is scrolling down. Always 0 on a plain 3-byte PS/2 mouse.
int     mouse_wheel_take(void);
int     mouse_has_wheel(void);

// True when the host reports real pointer coordinates rather than deltas,
// in which case our cursor lines up with the host's own.
int     mouse_is_absolute(void);
