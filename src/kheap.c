// src/kheap.c — first-fit free-list allocator over a static kernel heap
//
// The backing store is a plain .bss array, not a separately paging_map()'d
// region: it lives inside [kernel_start, kernel_end), so pmm_init() already
// reserves it and paging_init()'s identity map already covers it. That
// avoids a physical/virtual aliasing bug that a naive per-page pmm_alloc +
// paging_map would hit here (the frame a page is backed by need not equal
// its address). Once the kernel grows a real virtual address space this
// should become a demand-paged region instead of a fixed-size reservation.
#include <stdint.h>
#include <stddef.h>
#include "header/kheap.h"
#include "header/kprintf.h"
#include "header/irqctl.h"

// 16 MB. The browser is the demanding client: a TCP receive buffer, the
// downloaded page source, and the parsed node array all come from here, and
// they have to fit alongside everything else inside the project's 128 MB
// budget.
#define HEAP_SIZE (16u * 1024u * 1024u)

static uint8_t heap_area[HEAP_SIZE] __attribute__((aligned(16)));

// Padded to 16 bytes so every payload comes back 16-byte aligned. The header
// sits immediately before the pointer it hands out, so a 12-byte header meant
// every allocation was 4-byte aligned -- not enough for a uint64_t field in a
// kmalloc'd struct, and a trap waiting for the first one that appears.
// Request sizes are rounded to 16 for the same reason: a split block's next
// header has to land aligned too.
typedef struct block_header {
    size_t size;                 // usable bytes following this header
    int    free;
    struct block_header* next;
    uint32_t reserved;           // padding to 16 bytes; not used
} block_header_t;

static block_header_t* heap_head = 0;

void kheap_init(void){
    heap_head = (block_header_t*)heap_area;
    heap_head->size = HEAP_SIZE - sizeof(block_header_t);
    heap_head->free = 1;
    heap_head->next = 0;
    kprintf("[kheap] %u KB heap ready\n", HEAP_SIZE / 1024u);
}

static void split_block(block_header_t* b, size_t size){
    if (b->size >= size + sizeof(block_header_t) + 16u) {
        block_header_t* n = (block_header_t*)((uint8_t*)(b + 1) + size);
        n->size = b->size - size - sizeof(block_header_t);
        n->free = 1;
        n->next = b->next;
        b->size = size;
        b->next = n;
    }
}

// The free list is global mutable state, so once the scheduler can preempt a
// task mid-walk (or an interrupt handler allocates), these have to run with
// interrupts off. The list is short and the critical section is bounded, so a
// plain interrupt-disable is cheaper and simpler here than a real lock.
void* kmalloc(size_t size){
    if (size == 0) return 0;
    size = (size + 15u) & ~(size_t)15u;   // keeps payloads and split headers 16-byte aligned

    uint32_t f = irq_save();
    for (block_header_t* b = heap_head; b; b = b->next) {
        if (b->free && b->size >= size) {
            split_block(b, size);
            b->free = 0;
            irq_restore(f);
            return (void*)(b + 1);
        }
    }
    irq_restore(f);
    return 0;   // heap exhausted
}

static void coalesce(void){
    for (block_header_t* b = heap_head; b && b->next; b = b->next) {
        if (b->free && b->next->free) {
            b->size += sizeof(block_header_t) + b->next->size;
            b->next = b->next->next;
        }
    }
}

void kfree(void* ptr){
    if (!ptr) return;
    uint32_t f = irq_save();
    block_header_t* b = (block_header_t*)ptr - 1;
    b->free = 1;
    coalesce();
    irq_restore(f);
}

void kheap_stats(size_t* used_bytes, size_t* free_bytes){
    size_t used = 0, free_b = 0;
    uint32_t f = irq_save();
    for (block_header_t* b = heap_head; b; b = b->next) {
        if (b->free) free_b += b->size;
        else used += b->size;
    }
    irq_restore(f);
    if (used_bytes) *used_bytes = used;
    if (free_bytes) *free_bytes = free_b;
}
