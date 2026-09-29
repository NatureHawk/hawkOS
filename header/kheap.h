#pragma once
#include <stddef.h>

void  kheap_init(void);
void* kmalloc(size_t size);
void  kfree(void* ptr);
void* krealloc(void* ptr, size_t size);
size_t kheap_capacity(void);
int   kheap_check(void);      // 0 if every block header is consistent
void  kheap_stats(size_t* used_bytes, size_t* free_bytes);

// Requests of 256 KB or more are served by kmalloc automatically from
// contiguous frames rather than the heap array (kfree/krealloc accept either).
// The page-granular path is also available directly: page-aligned, rounded up
// to whole pages, not zeroed.
void*  kmalloc_pages(size_t size);
void   kfree_pages(void* ptr);
size_t kheap_large_bytes(void);   // bytes currently held by page allocations
