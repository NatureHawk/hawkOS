// src/lib_stbimage.c — builds the vendored stb_image decoder into the kernel
//
// stb_image (third_party/stb, public domain / MIT) decodes the formats the web
// actually serves images in: JPEG, PNG, GIF and BMP. The browser hands it the
// bytes of a fetched image and gets back 8-bit RGBA, which image.c turns into
// the 0x00RRGGBB the framebuffer wants.
//
// Only in-memory decoding is compiled: no stdio, no float HDR paths, no SIMD
// (the kernel is built without SSE code generation). Allocation goes to the
// kernel heap, and a failed assertion becomes a failed decode rather than a
// halted machine -- a malformed image on a web page is not a kernel bug.
#include "header/kheap.h"

#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_ASSERT(x)        ((void)0)
#define STBI_MALLOC(sz)       kmalloc(sz)
#define STBI_REALLOC(p, sz)   krealloc(p, sz)
#define STBI_FREE(p)          kfree(p)

#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb/stb_image.h"
