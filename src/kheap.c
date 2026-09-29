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
#include "header/pmm.h"

// 64 MB of the machine's 128. It started at 16, sized for one page of HTML,
// and a browser that ran in 10 MB left most of RAM doing nothing. What fills
// it now: the HTTP cache, decoded images (4 bytes a pixel, and a page can
// have dozens), and the video player's stream buffer and frames. What is left
// of the 128 MB after the kernel image goes to page tables and user processes
// through the frame allocator.
#define HEAP_SIZE (64u * 1024u * 1024u)

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
    uint32_t owner;              // return address of the kmalloc caller, for kheap_check
} block_header_t;

static block_header_t* heap_head = 0;

// Requests this big skip the free list and take contiguous frames straight
// from the frame allocator (identity-mapped, so the address is directly
// usable). The video ring, decoded frames and image caches are the callers;
// carved out of the 64 MB array they left it in pieces no later big request
// could use. Only the address and page count are kept, in a small table --
// frees and reallocs find the allocation by pointer.
#define LARGE_MIN  (256u * 1024u)
#define LARGE_MAX  128

static struct { uint32_t addr; uint32_t pages; } large[LARGE_MAX];
static uint32_t large_pages = 0;
static int      heap_dirty  = 1;      // any alloc/free since the last kheap_check

static int in_heap(const void* p){
    return (const uint8_t*)p >= heap_area && (const uint8_t*)p < heap_area + HEAP_SIZE;
}

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
void* kmalloc_pages(size_t size){
    if (size == 0) return 0;
    uint32_t n = (uint32_t)((size + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE);
    uint32_t f = irq_save();
    for (int i = 0; i < LARGE_MAX; i++){
        if (large[i].addr) continue;
        void* p = pmm_alloc_contig(n);
        if (p){
            large[i].addr  = (uint32_t)p;
            large[i].pages = n;
            large_pages   += n;
        }
        irq_restore(f);
        return p;
    }
    irq_restore(f);
    return 0;
}

// Returns 1 if `ptr` was a page allocation and has been released.
static int free_large(void* ptr){
    uint32_t f = irq_save();
    for (int i = 0; i < LARGE_MAX; i++){
        if (large[i].addr == (uint32_t)ptr && ptr){
            pmm_free_contig(ptr, large[i].pages);
            large_pages -= large[i].pages;
            large[i].addr = 0;
            irq_restore(f);
            return 1;
        }
    }
    irq_restore(f);
    return 0;
}

void kfree_pages(void* ptr){ if (ptr) free_large(ptr); }

void* kmalloc(size_t size){
    if (size == 0) return 0;
    if (size >= LARGE_MIN){
        void* p = kmalloc_pages(size);
        if (p) return p;              // out of frames or table: try the array
    }
    size = (size + 15u) & ~(size_t)15u;   // keeps payloads and split headers 16-byte aligned

    uint32_t f = irq_save();
    heap_dirty = 1;
    for (block_header_t* b = heap_head; b; b = b->next) {
        if (b->free && b->size >= size) {
            split_block(b, size);
            b->free = 0;
            b->owner = (uint32_t)(uintptr_t)__builtin_return_address(0);
            irq_restore(f);
            return (void*)(b + 1);
        }
    }
    irq_restore(f);
    return 0;   // heap exhausted
}

// Stays on a block after merging into it, so a run of three or more free
// neighbours collapses into one in a single pass instead of leaving every
// other seam behind -- which is what kept multi-megabyte requests (a stream
// buffer, a decoded image) failing on a heap that was mostly free.
static void coalesce(void){
    block_header_t* b = heap_head;
    while (b && b->next) {
        if (b->free && b->next->free) {
            b->size += sizeof(block_header_t) + b->next->size;
            b->next = b->next->next;
        } else {
            b = b->next;
        }
    }
}

void kfree(void* ptr){
    if (!ptr) return;
    if (!in_heap(ptr)){ free_large(ptr); return; }
    uint32_t f = irq_save();
    heap_dirty = 1;
    block_header_t* b = (block_header_t*)ptr - 1;
    b->free = 1;
    coalesce();
    irq_restore(f);
}

// Grows in place when the following block is free and big enough, which is
// the common case for a buffer being appended to; otherwise moves.
void* krealloc(void* ptr, size_t size){
    if (!ptr) return kmalloc(size);
    if (size == 0){ kfree(ptr); return 0; }
    size = (size + 15u) & ~(size_t)15u;

    if (!in_heap(ptr)){
        uint32_t have = 0;
        for (int i = 0; i < LARGE_MAX; i++)
            if (large[i].addr == (uint32_t)ptr) have = large[i].pages * PMM_FRAME_SIZE;
        if (!have) return 0;
        if (size <= have) return ptr;
        void* q = kmalloc(size);
        if (!q) return 0;
        uint8_t* d = (uint8_t*)q;
        const uint8_t* s = (const uint8_t*)ptr;
        for (uint32_t i = 0; i < have; i++) d[i] = s[i];
        kfree(ptr);
        return q;
    }

    block_header_t* b = (block_header_t*)ptr - 1;
    if (b->size >= size) return ptr;

    uint32_t f = irq_save();
    heap_dirty = 1;
    block_header_t* n = b->next;
    if (n && n->free && (uint8_t*)n == (uint8_t*)(b + 1) + b->size
        && b->size + sizeof(block_header_t) + n->size >= size){
        b->size += sizeof(block_header_t) + n->size;
        b->next  = n->next;
        split_block(b, size);
        irq_restore(f);
        return ptr;
    }
    irq_restore(f);

    void* p = kmalloc(size);
    if (!p) return 0;
    uint8_t* d = (uint8_t*)p;
    const uint8_t* s = (const uint8_t*)ptr;
    for (size_t i = 0; i < b->size; i++) d[i] = s[i];
    kfree(ptr);
    return p;
}

size_t kheap_capacity(void){ return HEAP_SIZE; }

// Walks the block list and checks every header is where the one before it
// says it should be. A write past the end of an allocation lands on the next
// block's header, and the first sign of it is otherwise much later and far
// away -- a heap that suddenly looks a few hundred KB long. Reports the block
// before the damage, and who allocated it, once.
//
// Only allocation and free change the block list, so a check with nothing
// having moved since the last clean one has nothing new to find. The window
// manager polls this once a second; on an idle heap that is now a flag test
// instead of a walk of every block.
int kheap_check(void){
    static int reported = 0;
    if (!heap_dirty) return 0;
    uint8_t* lo = heap_area;
    uint8_t* hi = heap_area + HEAP_SIZE;
    uint32_t f = irq_save();
    heap_dirty = 0;
    block_header_t* prev = 0;
    int bad = 0;
    for (block_header_t* b = heap_head; b; b = b->next){
        uint8_t* end = (uint8_t*)(b + 1) + b->size;
        int ok = (uint8_t*)b >= lo && end <= hi && (b->free == 0 || b->free == 1)
              && (b->next ? (uint8_t*)b->next == end : end == hi);
        if (!ok){
            bad = 1;
            if (!reported){
                reported = 1;
                kprintf("[kheap] CORRUPT header at %p: size=%x free=%x next=%p owner=%p\n",
                        (void*)b, (uint32_t)b->size, (uint32_t)b->free, (void*)b->next, (void*)b->owner);
                if (prev)
                    kprintf("[kheap]   previous block %p size=%u free=%d owner=%p\n",
                            (void*)prev, (uint32_t)prev->size, prev->free, (void*)prev->owner);
            }
            break;
        }
        prev = b;
    }
    if (bad) heap_dirty = 1;      // keep reporting -1 until the damage is gone
    irq_restore(f);
    return bad ? -1 : 0;
}

// Bytes held by page allocations. They are frames, so the frame allocator's
// own free count already reflects them; this is for whoever wants the split.
size_t kheap_large_bytes(void){ return (size_t)large_pages * PMM_FRAME_SIZE; }

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
