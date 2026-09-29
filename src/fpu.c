// src/fpu.c — x87/SSE enable and context save
#include <stdint.h>
#include "header/fpu.h"
#include "header/kstring.h"
#include "header/kprintf.h"

#define CR0_MP  (1u << 1)     // WAIT/FWAIT honours TS
#define CR0_EM  (1u << 2)     // set = no FPU, every float op traps
#define CR0_TS  (1u << 3)     // task switched: next float op traps
#define CR0_NE  (1u << 5)     // report x87 errors as #MF, not through the PIC

#define CR4_OSFXSR     (1u << 9)    // fxsave/fxrstor and SSE instructions allowed
#define CR4_OSXMMEXCPT (1u << 10)   // unmasked SSE exceptions raise #XM

static int has_fxsr = 0;

// A freshly initialised FPU, captured once so a new task starts from exactly
// what fninit would have given it, without having to run fninit on its behalf
// in the middle of a switch.
static uint8_t clean_state[FPU_STATE_SIZE] __attribute__((aligned(16)));

static void cpuid1(uint32_t* ecx, uint32_t* edx){
    uint32_t a = 1, b, c = 0, d;
    __asm__ __volatile__("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    *ecx = c; *edx = d;
}

void fpu_init(void){
    uint32_t ecx, edx;
    cpuid1(&ecx, &edx);
    (void)ecx;

    int have_fpu = (edx & 1u) != 0;
    has_fxsr     = (edx & (1u << 24)) != 0;
    int have_sse = (edx & (1u << 25)) != 0;

    uint32_t cr0;
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(CR0_EM | CR0_TS);
    cr0 |= CR0_MP | CR0_NE;
    __asm__ __volatile__("mov %0, %%cr0" :: "r"(cr0));

    if (has_fxsr){
        uint32_t cr4;
        __asm__ __volatile__("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= CR4_OSFXSR;
        if (have_sse) cr4 |= CR4_OSXMMEXCPT;
        __asm__ __volatile__("mov %0, %%cr4" :: "r"(cr4));
    }

    __asm__ __volatile__("fninit");
    memset(clean_state, 0, sizeof(clean_state));
    fpu_save(clean_state);
    // fnsave reinitialises the FPU as a side effect; fxsave does not. Either
    // way the boot task carries on from a clean state.
    fpu_restore(clean_state);

    kprintf("[fpu] %s, %s save\n", have_fpu ? "x87 present" : "no x87 reported",
            has_fxsr ? (have_sse ? "fxsave+SSE" : "fxsave") : "fnsave");
}

int fpu_has_fxsr(void){ return has_fxsr; }

void fpu_init_state(uint8_t* area){
    memcpy(area, clean_state, FPU_STATE_SIZE);
}

void fpu_save(uint8_t* area){
    if (has_fxsr) __asm__ __volatile__("fxsave (%0)" :: "r"(area) : "memory");
    else          __asm__ __volatile__("fnsave (%0)" :: "r"(area) : "memory");
}

void fpu_restore(const uint8_t* area){
    if (has_fxsr) __asm__ __volatile__("fxrstor (%0)" :: "r"(area) : "memory");
    else          __asm__ __volatile__("frstor (%0)"  :: "r"(area) : "memory");
}
