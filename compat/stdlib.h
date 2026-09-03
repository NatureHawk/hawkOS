// compat/stdlib.h — BearSSL includes this for size_t and (in code paths we
// do not compile) malloc. Nothing here allocates; the kernel's kmalloc is
// used explicitly where BearSSL needs a buffer.
#pragma once
#include <stddef.h>
