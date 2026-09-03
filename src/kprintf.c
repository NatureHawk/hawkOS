// src/kprintf.c — kernel printf
//
// The formatting itself lives in kstring.c's kvsnprintf, so there is exactly
// one implementation of %-handling in the kernel: fixing a conversion there
// (width, zero-padding, %X) fixes it for the shell, the GUI apps and the
// network log at once. This file only owns *where* the characters go.
#include <stdarg.h>
#include <stdint.h>
#include "console.h"
#include "serial.h"
#include "kprintf.h"
#include "header/kstring.h"

static kprintf_sink_t sink = 0;

void kprintf_set_sink(kprintf_sink_t fn){ sink = fn; }

static void outc(char c){
    if (sink) sink(c);
    else      console_putc(c);
    serial_putc(c);
}

int kprintf(const char *fmt, ...){
    // One line at a time. A fixed buffer keeps this allocation-free, which
    // matters because kprintf is called from paths (early boot, the heap
    // itself) where kmalloc is not available or not safe.
    char buf[512];

    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    for (const char* p = buf; *p; p++) outc(*p);
    return n;
}
