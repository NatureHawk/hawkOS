// src/lib_plmpeg.c — builds the vendored PL_MPEG decoder into the kernel
//
// PL_MPEG (third_party/pl_mpeg, MIT) is an MPEG-1 video / MP2 audio decoder
// and MPEG-PS demuxer in one header. It is here because a from-scratch video
// decoder is a project of its own, and MPEG-1 is the codec a machine without
// a GPU can still decode in real time: every patent on it has expired and it
// needs nothing but integer IDCTs and a float MP2 synthesis filter.
//
// Everything it would get from libc is routed to the kernel: the allocator
// through PLM_MALLOC and friends, and stdio switched off entirely -- data
// arrives through a plm_buffer the video engine fills from the network.
#include "header/kheap.h"

#define PLM_NO_STDIO
#define PLM_MALLOC(sz)      kmalloc(sz)
#define PLM_FREE(p)         kfree(p)
#define PLM_REALLOC(p, sz)  krealloc(p, sz)

#define PL_MPEG_IMPLEMENTATION
#include "third_party/pl_mpeg/pl_mpeg.h"
