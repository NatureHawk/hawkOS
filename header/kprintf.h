#ifndef KPRINTF_H
#define KPRINTF_H
#include <stdarg.h>

int kprintf(const char *fmt, ...);

// Redirect kprintf's screen output. With a sink installed, characters go to
// it instead of the raw framebuffer text console — which is what lets the
// GUI terminal window reuse every existing shell command unchanged instead
// of having them scribble over the desktop. Serial output is never
// redirected, so the boot log stays intact either way. Pass 0 to restore.
typedef void (*kprintf_sink_t)(char c);
void kprintf_set_sink(kprintf_sink_t fn);

#endif
