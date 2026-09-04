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

// ---------------------------------------------------------- address spaces

uint32_t paging_kernel_dir(void){ return (uint32_t)page_directory; }

void paging_switch(uint32_t pd){
    __asm__ __volatile__("mov %0, %%cr3" :: "r"(pd) : "memory");
}

// A fresh directory that shares the kernel's mappings and nothing else.
//
// Copying the kernel's directory entries rather than its page tables is what
// keeps the sharing honest: every space points at the same tables, so a later
// kernel mapping appears in all of them at once, and there is one copy of the
// kernel rather than one per process. Those pages carry no USER bit, so ring 3
// cannot touch them even though they are mapped.
uint32_t paging_new_address_space(void){
    uint32_t* pd = (uint32_t*)pmm_alloc_frame();
    if (!pd) return 0;

    for (int i = 0; i < PD_ENTRIES; i++) pd[i] = page_directory[i];
    return (uint32_t)pd;
}

// Releases a directory and the page tables it alone created. Entries shared
// with the kernel are left alone: they belong to every other address space
// too, and freeing them here would unmap the kernel from under itself.
void paging_free_address_space(uint32_t pd_phys){
    if (!pd_phys || pd_phys == (uint32_t)page_directory) return;
    uint32_t* pd = (uint32_t*)pd_phys;

    for (int i = 0; i < PD_ENTRIES; i++){
        if (!(pd[i] & PAGE_PRESENT)) continue;
        if (pd[i] == page_directory[i]) continue;      // shared with the kernel
        pmm_free_frame((void*)(pd[i] & 0xFFFFF000u));
    }
    pmm_free_frame((void*)pd_phys);
}

static uint32_t* table_in(uint32_t pd_phys, uint32_t pd_index, int create){
    uint32_t* pd = (uint32_t*)pd_phys;

    if (pd[pd_index] & PAGE_PRESENT){
        // A table inherited from the kernel must not be written through: it is
        // shared with every other address space, so adding a user page to it
        // would add that page to all of them. Copy it first.
        if (pd[pd_index] == page_directory[pd_index] && create){
            uint32_t* fresh = (uint32_t*)pmm_alloc_frame();
            if (!fresh) return 0;
            const uint32_t* old = (const uint32_t*)(pd[pd_index] & 0xFFFFF000u);
            for (int i = 0; i < PT_ENTRIES; i++) fresh[i] = old[i];
            pd[pd_index] = ((uint32_t)fresh & 0xFFFFF000u)
                         | PAGE_PRESENT | PAGE_RW | PAGE_USER;
        }
        return (uint32_t*)(pd[pd_index] & 0xFFFFF000u);
    }

    if (!create) return 0;

    uint32_t* table = (uint32_t*)pmm_alloc_frame();
    if (!table) return 0;
    for (int i = 0; i < PT_ENTRIES; i++) table[i] = 0;

    // The directory entry has to permit user access for the entries below it
    // to be reachable at all: the CPU takes the most restrictive of the two
    // levels, so a supervisor-only directory entry hides a user page.
    pd[pd_index] = ((uint32_t)table & 0xFFFFF000u) | PAGE_PRESENT | PAGE_RW | PAGE_USER;
    return table;
}

void paging_map_in(uint32_t pd, uint32_t phys, uint32_t virt, uint32_t flags){
    uint32_t* table = table_in(pd, virt >> 22, 1);
    if (!table) return;
    table[(virt >> 12) & 0x3FFu] = (phys & 0xFFFFF000u) | (flags & 0xFFFu) | PAGE_PRESENT;
}

uint32_t paging_phys_of(uint32_t pd, uint32_t virt){
    uint32_t* table = table_in(pd, virt >> 22, 0);
    if (!table) return 0;
    uint32_t e = table[(virt >> 12) & 0x3FFu];
    if (!(e & PAGE_PRESENT)) return 0;
    return e & 0xFFFFF000u;
}
