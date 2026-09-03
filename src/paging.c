// src/paging.c — flat identity-mapped paging (no higher-half kernel yet)
#include <stdint.h>
#include "header/paging.h"
#include "header/pmm.h"
#include "header/kprintf.h"

#define PD_ENTRIES 1024
#define PT_ENTRIES 1024

static uint32_t* page_directory = 0;

static uint32_t* get_or_create_table(uint32_t pd_index){
    if (page_directory[pd_index] & PAGE_PRESENT)
        return (uint32_t*)(page_directory[pd_index] & 0xFFFFF000u);

    uint32_t* table = (uint32_t*)pmm_alloc_frame();
    if (!table){
        kprintf("[paging] out of memory allocating page table\n");
        for(;;) __asm__ __volatile__("hlt");
    }
    for (int i = 0; i < PT_ENTRIES; i++) table[i] = 0;
    page_directory[pd_index] = ((uint32_t)table & 0xFFFFF000u) | PAGE_PRESENT | PAGE_RW;
    return table;
}

void paging_map(uint32_t phys, uint32_t virt, uint32_t flags){
    uint32_t pd_index = virt >> 22;
    uint32_t pt_index = (virt >> 12) & 0x3FFu;
    uint32_t* table = get_or_create_table(pd_index);
    table[pt_index] = (phys & 0xFFFFF000u) | (flags & 0xFFFu) | PAGE_PRESENT;
}

void paging_identity_map_range(uint32_t phys_start, uint32_t phys_end, uint32_t flags){
    phys_start &= 0xFFFFF000u;
    for (uint32_t addr = phys_start; addr < phys_end; addr += PMM_FRAME_SIZE)
        paging_map(addr, addr, flags);
}

void paging_init(void){
    page_directory = (uint32_t*)pmm_alloc_frame();
    if (!page_directory){
        kprintf("[paging] out of memory allocating page directory\n");
        for(;;) __asm__ __volatile__("hlt");
    }
    for (int i = 0; i < PD_ENTRIES; i++) page_directory[i] = 0;

    uint32_t top = pmm_ram_top();
    if (top == 0) top = 16u * 1024u * 1024u;   // paranoia fallback, shouldn't happen

    // Identity map everything the PMM knows about. Every page table this
    // allocates comes from the same pool, so by construction it also ends up
    // identity-mapped by the time this loop reaches its own frame.
    paging_identity_map_range(0, top, PAGE_RW);

    __asm__ __volatile__(
        "mov %0, %%cr3\n\t"
        "mov %%cr0, %%eax\n\t"
        "or  $0x80000000, %%eax\n\t"
        "mov %%eax, %%cr0\n\t"
        :: "r"(page_directory)
        : "eax", "memory"
    );

    kprintf("[paging] enabled, identity-mapped 0..%u MB\n", top / (1024u * 1024u));
}
