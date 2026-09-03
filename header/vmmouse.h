#pragma once
#include <stdint.h>

// VMware absolute-pointer protocol, which QEMU emulates as its "vmmouse"
// device alongside the PS/2 port.
//
// A PS/2 mouse only ever reports "moved this far since last time". The host
// draws its own cursor from the real pointer while the guest draws one from
// accumulated deltas, and the two drift apart for good the moment the guest
// clamps at a screen edge while the host pointer keeps travelling. No amount
// of tuning fixes that; the guest has to be told where the pointer actually
// is.
//
// This protocol does exactly that: a magic `in` from port 0x5658 returns
// absolute coordinates in a 0..65535 box. The device still raises IRQ12
// through the PS/2 aux channel, so it slots in under the existing handler.
// When it is active QEMU also knows the guest has an absolute pointer, stops
// grabbing, and lines its own cursor up with ours.

int vmmouse_detect(void);          // is the vmport there at all
int vmmouse_enable(void);          // 0 on success, -1 if unavailable
void vmmouse_disable(void);

// Drains one packet. Returns 1 and fills the outputs if there was one, 0 if
// the queue was empty, -1 if the device faulted (caller should fall back to
// plain PS/2). x and y come back in the raw 0..65535 space.
int vmmouse_poll(uint32_t* x, uint32_t* y, int* buttons, int* wheel);

#define VMMOUSE_LEFT   0x20
#define VMMOUSE_RIGHT  0x10
#define VMMOUSE_MIDDLE 0x08
