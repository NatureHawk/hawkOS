#pragma once
#include <stdint.h>

#define PAGE_PRESENT 0x1u
#define PAGE_RW      0x2u
#define PAGE_USER    0x4u

// Sets up a page directory, identity-maps all physical RAM the PMM knows
// about, and turns paging on. Must run after pmm_init().
void paging_init(void);

// Maps one 4KB page. Allocates a page table from the PMM on demand if the
// covering page directory entry isn't present yet.
void paging_map(uint32_t phys, uint32_t virt, uint32_t flags);

void paging_identity_map_range(uint32_t phys_start, uint32_t phys_end, uint32_t flags);
