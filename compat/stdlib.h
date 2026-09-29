// compat/stdlib.h — BearSSL includes this for size_t and (in code paths we
// do not compile) malloc. Nothing here allocates; the kernel's kmalloc is
// used explicitly where BearSSL needs a buffer.
#pragma once
#include <stddef.h>

// abs() for the vendored media decoders (pl_mpeg, stb_image), which are the
// only third-party code that wants anything from here beyond size_t.
static inline int abs(int v){ return v < 0 ? -v : v; }
