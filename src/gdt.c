// src/gdt.c — GDT with ring-3 segments and a task state segment
//
// Six descriptors: null, kernel code/data, user code/data, and the TSS.
//
// The TSS is what makes ring 3 possible at all. When an interrupt arrives
// while the CPU is at CPL 3 it cannot keep using the user stack -- the kernel
// would be running on memory the user program controls -- so the CPU reads a
// stack pointer out of the TSS and switches to it before pushing anything.
// That is the only field of the TSS this kernel uses; hardware task switching,
// the rest of the structure's original purpose, is not used by anything.
#include <stdint.h>
#include "header/gdt.h"
#include "header/kstring.h"
#include "header/kprintf.h"

typedef struct __attribute__((packed)) {
    uint16_t limit_lo;
    uint16_t base_lo;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;   // high 4 bits of limit, plus the flag nibble
    uint8_t  base_hi;
} gdt_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint32_t base;
} gdt_ptr_t;

// Only the fields the CPU reads on a privilege change matter here; the rest
// exist because the structure has a fixed layout.
typedef struct __attribute__((packed)) {
    uint32_t prev_tss;
    uint32_t esp0;          // stack to switch to on a ring 3 -> ring 0 trap
    uint32_t ss0;
    uint32_t esp1, ss1, esp2, ss2;
    uint32_t cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} tss_t;

#define GDT_ENTRIES 6

static gdt_entry_t gdt[GDT_ENTRIES];
static gdt_ptr_t   gdtp;
static tss_t       tss;

extern void gdt_flush(uint32_t gdtp_addr);
extern void tss_flush(uint32_t selector);

static void set_entry(int i, uint32_t base, uint32_t limit,
                      uint8_t access, uint8_t flags){
    gdt[i].limit_lo    = (uint16_t)(limit & 0xFFFFu);
    gdt[i].base_lo     = (uint16_t)(base & 0xFFFFu);
    gdt[i].base_mid    = (uint8_t)((base >> 16) & 0xFFu);
    gdt[i].access      = access;
    gdt[i].granularity = (uint8_t)(((limit >> 16) & 0x0Fu) | (flags & 0xF0u));
    gdt[i].base_hi     = (uint8_t)((base >> 24) & 0xFFu);
}

void tss_set_kernel_stack(uint32_t esp0){
    tss.esp0 = esp0;
}

void gdt_init(void){
    // Access byte: P DPL S | Type. 0x9A is ring-0 code, 0x92 ring-0 data;
    // 0xFA and 0xF2 are the same with DPL 3. Granularity 0xCF is 4 KB pages
    // and 32-bit operands, giving each segment the full 4 GB.
    set_entry(0, 0, 0,          0x00, 0x00);
    set_entry(1, 0, 0x000FFFFF, 0x9A, 0xCF);   // 0x08 kernel code
    set_entry(2, 0, 0x000FFFFF, 0x92, 0xCF);   // 0x10 kernel data
    set_entry(3, 0, 0x000FFFFF, 0xFA, 0xCF);   // 0x18 user code, DPL 3
    set_entry(4, 0, 0x000FFFFF, 0xF2, 0xCF);   // 0x20 user data, DPL 3

    memset(&tss, 0, sizeof(tss));
    tss.ss0  = SEL_KDATA;
    tss.esp0 = 0;                      // filled in per task switch
    // Past the end of the segment: no I/O permission bitmap, so every port
    // access from ring 3 faults instead of being silently allowed.
    tss.iomap_base = sizeof(tss);

    // 0x89 is a present, ring-0, 32-bit available TSS descriptor. The limit
    // is the structure's last byte, not its size.
    set_entry(5, (uint32_t)&tss, sizeof(tss) - 1, 0x89, 0x00);

    gdtp.limit = sizeof(gdt) - 1;
    gdtp.base  = (uint32_t)&gdt[0];

    gdt_flush((uint32_t)&gdtp);
    tss_flush(SEL_TSS);
}
