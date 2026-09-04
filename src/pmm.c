// src/pmm.c — bitmap physical frame allocator
#include <stdint.h>
#include "header/pmm.h"
#include "header/multiboot.h"
#include "header/kheap.h"
#include "header/kprintf.h"

// Static tracking capacity. Sized generously above the 128MB target so real
// runs never hit the cap; anything beyond this is simply never handed out.
#define MAX_PHYS_MB   256u
#define MAX_FRAMES    ((MAX_PHYS_MB * 1024u * 1024u) / PMM_FRAME_SIZE)   // 65536
#define BITMAP_BYTES  (MAX_FRAMES / 8u)                                   // 8192

extern uint8_t kernel_start[];
extern uint8_t kernel_end[];

static uint8_t  bitmap[BITMAP_BYTES];
static uint32_t free_count = 0;
static uint32_t ram_top    = 0;   // highest byte address of usable RAM we've seen
static uint32_t scan_hint  = 0;   // next-fit cursor so alloc isn't O(n) from frame 0 every time

static inline void     set_bit(uint32_t f)   { bitmap[f >> 3] |=  (uint8_t)(1u << (f & 7)); }
static inline void     clear_bit(uint32_t f) { bitmap[f >> 3] &= (uint8_t)~(1u << (f & 7)); }
static inline int      test_bit(uint32_t f)  { return (bitmap[f >> 3] >> (f & 7)) & 1; }

static void mark_used(uint32_t frame){
    if (frame >= MAX_FRAMES) return;
    if (!test_bit(frame)) { set_bit(frame); free_count--; }
}

static void mark_free(uint32_t frame){
    if (frame >= MAX_FRAMES) return;
    if (test_bit(frame)) {
        clear_bit(frame);
        free_count++;
        uint32_t top = (frame + 1) * PMM_FRAME_SIZE;
        if (top > ram_top) ram_top = top;
    }
}

static void mark_used_range(uint32_t start, uint32_t end){
    if (end <= start) return;
    uint32_t f_start = start / PMM_FRAME_SIZE;
    uint32_t f_end   = (end + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
    for (uint32_t f = f_start; f < f_end; f++) mark_used(f);
}

static void mark_free_range(uint64_t start, uint64_t end){
    if (end > 0xFFFFFFFFull) end = 0xFFFFFFFFull;
    if (start >= end) return;
    uint32_t f_start = (uint32_t)(start / PMM_FRAME_SIZE);
    uint32_t f_end   = (uint32_t)(end   / PMM_FRAME_SIZE);
    for (uint32_t f = f_start; f < f_end; f++) mark_free(f);
}

void pmm_init(uint32_t magic, uint32_t mbi_addr){
    // Start fully reserved; only regions the bootloader reports as usable
    // (or our fallback guess) get cleared below.
    for (uint32_t i = 0; i < BITMAP_BYTES; i++) bitmap[i] = 0xFF;
    free_count = 0;
    ram_top    = 0;

    const multiboot_info_t* mbi =
        (magic == MULTIBOOT_BOOTLOADER_MAGIC) ? (const multiboot_info_t*)mbi_addr : 0;

    if (mbi && (mbi->flags & MULTIBOOT_FLAG_MMAP) && mbi->mmap_length) {
        uint32_t off = 0;
        while (off < mbi->mmap_length) {
            const multiboot_mmap_entry_t* e =
                (const multiboot_mmap_entry_t*)(mbi->mmap_addr + off);
            if (e->type == MULTIBOOT_MEMORY_AVAILABLE)
                mark_free_range(e->addr, e->addr + e->len);
            off += e->size + (uint32_t)sizeof(e->size);
        }
        kprintf("[pmm] using multiboot memory map\n");
    } else if (mbi && (mbi->flags & MULTIBOOT_FLAG_MEM)) {
        mark_free_range(0, (uint64_t)mbi->mem_lower * 1024u);
        mark_free_range(1024u * 1024u,
                         1024ull * 1024u + (uint64_t)mbi->mem_upper * 1024u);
        kprintf("[pmm] no mmap; using mem_lower/mem_upper\n");
    } else {
        // last resort: assume 16MB usable above the 1MB mark
        mark_free_range(1024u * 1024u, 16ull * 1024u * 1024u);
        kprintf("[pmm] no multiboot memory info; guessing 16MB\n");
    }

    // Reserve the first 1MB unconditionally: IVT, EBDA, VGA memory, BIOS ROMs.
    mark_used(0);
    uint32_t first_mb_frames = (1024u * 1024u) / PMM_FRAME_SIZE;
    for (uint32_t f = 0; f < first_mb_frames; f++) mark_used(f);

    // Reserve the kernel image itself so the allocator never hands it out.
    uint32_t k_start = (uint32_t)kernel_start / PMM_FRAME_SIZE;
    uint32_t k_end   = ((uint32_t)kernel_end + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
    for (uint32_t f = k_start; f < k_end; f++) mark_used(f);

    // And the bootloader's own structures, which live in ordinary RAM the
    // allocator would otherwise hand straight out.
    //
    // This is not hypothetical tidiness. QEMU's multiboot loader places the
    // info block, the command line and the memory map immediately above the
    // kernel image -- which is precisely where paging_init()'s page tables
    // get allocated from, since they are the first thing to ask for frames.
    // The boot command line was being overwritten before anything could read
    // it. GRUB happens to put its copies in low memory, which is why this
    // never showed up when booting the ISO.
    if (mbi){
        mark_used_range((uint32_t)mbi, (uint32_t)mbi + (uint32_t)sizeof(*mbi));

        if ((mbi->flags & MULTIBOOT_FLAG_CMDLINE) && mbi->cmdline){
            const char* s = (const char*)mbi->cmdline;
            uint32_t n = 0;
            while (n < 4096u && s[n]) n++;
            mark_used_range(mbi->cmdline, mbi->cmdline + n + 1);
        }
        if ((mbi->flags & MULTIBOOT_FLAG_MMAP) && mbi->mmap_length)
            mark_used_range(mbi->mmap_addr, mbi->mmap_addr + mbi->mmap_length);
    }

    kprintf("[pmm] %u KB free / %u KB tracked, ram_top=0x%x\n",
            free_count * (PMM_FRAME_SIZE / 1024u),
            MAX_FRAMES * (PMM_FRAME_SIZE / 1024u),
            ram_top);
}

void* pmm_alloc_frame(void){
    for (uint32_t i = 0; i < MAX_FRAMES; i++) {
        uint32_t f = (scan_hint + i) % MAX_FRAMES;
        if (!test_bit(f)) {
            set_bit(f);
            free_count--;
            scan_hint = f + 1;
            return (void*)(f * PMM_FRAME_SIZE);
        }
    }
    return 0;
}

void pmm_free_frame(void* frame_phys){
    uint32_t f = (uint32_t)frame_phys / PMM_FRAME_SIZE;
    mark_free(f);
}

uint32_t pmm_total_frames(void){ return MAX_FRAMES; }
uint32_t pmm_free_frames(void){ return free_count; }
uint32_t pmm_ram_top(void){ return ram_top; }

uint32_t pmm_total_kb(void){ return ram_top / 1024u; }

// Frames the allocator still has, plus the part of the kernel heap it already
// counted as used but that kmalloc has not handed out. Without that second
// term the figure is frozen at boot -- see the note in header/pmm.h.
uint32_t pmm_used_kb(void){
    size_t heap_used = 0, heap_free = 0;
    kheap_stats(&heap_used, &heap_free);

    uint32_t total_kb = ram_top / 1024u;
    uint32_t free_kb  = free_count * (PMM_FRAME_SIZE / 1024u)
                      + (uint32_t)(heap_free / 1024u);
    return free_kb < total_kb ? total_kb - free_kb : 0;
}
