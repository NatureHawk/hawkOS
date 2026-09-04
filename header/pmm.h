#pragma once
#include <stdint.h>

#define PMM_FRAME_SIZE 4096u

// magic/mbi_addr are exactly what boot.s captured from EAX/EBX at entry.
void pmm_init(uint32_t multiboot_magic, uint32_t multiboot_info_addr);

// Returns the physical address of a free 4KB frame, zero-filled bookkeeping
// only (contents are NOT cleared). Returns NULL if out of memory.
void* pmm_alloc_frame(void);
void  pmm_free_frame(void* frame_phys);

uint32_t pmm_total_frames(void);   // static bitmap capacity (tracking limit)
uint32_t pmm_free_frames(void);
uint32_t pmm_ram_top(void);        // highest physical address of detected usable RAM

// What anything reporting system memory to a user should call.
//
// Frame counts alone are the wrong answer: the kernel heap is a static array
// inside the kernel image, so pmm_init() marks the whole of it used before a
// byte has been handed out, and every dynamic allocation in the system goes
// through kmalloc, which the frame allocator never sees. A meter built on
// pmm_free_frames() alone shows the same number from boot to shutdown.
uint32_t pmm_used_kb(void);
uint32_t pmm_total_kb(void);
