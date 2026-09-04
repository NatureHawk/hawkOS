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

// ------------------------------------------------------- address spaces
//
// A process gets a directory of its own so that what it can reach is decided
// by which page tables its directory names, not by what it can guess. The
// kernel's directory entries are copied into every new space, but the pages
// they lead to carry no USER bit -- so kernel memory stays mapped (interrupts
// and system calls need it) and stays unreachable from ring 3.

uint32_t paging_kernel_dir(void);              // physical address of the kernel's directory
uint32_t paging_new_address_space(void);       // 0 on failure
void     paging_free_address_space(uint32_t pd);

// Maps one page in a directory that is not necessarily the active one.
void     paging_map_in(uint32_t pd, uint32_t phys, uint32_t virt, uint32_t flags);

// The physical frame behind a virtual address in `pd`, or 0 if unmapped.
uint32_t paging_phys_of(uint32_t pd, uint32_t virt);

// Loads CR3. Cheap, but not free: it flushes the TLB, so the scheduler only
// calls it when the address space actually changes.
void     paging_switch(uint32_t pd);
