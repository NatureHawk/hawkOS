// header/irqctl.h — save/restore the interrupt flag around a critical section.
//
// Plain cli/sti is wrong once code can be called from both task context and
// an interrupt handler: the sti would re-enable interrupts inside a handler
// that was entered with them off. These push the old EFLAGS instead, so a
// nested critical section leaves interrupts exactly as it found them.
#pragma once
#include <stdint.h>

static inline uint32_t irq_save(void){
    uint32_t f;
    __asm__ __volatile__("pushfl\n\tpopl %0\n\tcli" : "=r"(f) :: "memory");
    return f;
}

static inline void irq_restore(uint32_t f){
    __asm__ __volatile__("pushl %0\n\tpopfl" :: "r"(f) : "memory", "cc");
}
