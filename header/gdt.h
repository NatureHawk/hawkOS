#pragma once
#include <stdint.h>

// Segment selectors. The low two bits are the requested privilege level, so
// the ring-3 selectors carry a 3 and the ring-0 ones a 0 — a user segment
// register loaded with 0x18 rather than 0x1B would fault the moment the CPU
// checked it against CPL.
#define SEL_KCODE  0x08
#define SEL_KDATA  0x10
#define SEL_UCODE  (0x18 | 3)
#define SEL_UDATA  (0x20 | 3)
#define SEL_TSS    0x28

// Builds the GDT, loads it, and installs the TSS.
//
// The old flat GDT was assembled statically, which was fine while everything
// ran in ring 0 with one stack. A TSS cannot be: its descriptor holds the
// runtime address of a structure the kernel has to keep updating, so the
// table is built in C now.
void gdt_init(void);

// Points the TSS at the kernel stack an interrupt should switch to when it
// arrives while the CPU is in ring 3. This has to be updated on every context
// switch into a user task -- the CPU reads esp0 out of the TSS at the moment
// of the trap, and pointing it at the wrong task's stack corrupts both.
void tss_set_kernel_stack(uint32_t esp0);
