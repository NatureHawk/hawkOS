#pragma once
#include <stddef.h>

void  kheap_init(void);
void* kmalloc(size_t size);
void  kfree(void* ptr);
void  kheap_stats(size_t* used_bytes, size_t* free_bytes);
