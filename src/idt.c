#include <stdint.h>
#include "header/idt.h"
typedef struct __attribute__((packed)) {
    uint16_t base_lo;
    uint16_t sel;
    uint8_t  zero;
    uint8_t  flags;
    uint16_t base_hi;
} idt_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint32_t base;
} idtr_t;

static idt_entry_t idt[256];
static idtr_t idtr;

extern void isr_stub(void);
extern void idt_load(void*);
extern void irq0_stub(void);
extern void irq1_stub(void);

void set_gate(int n, uint32_t h) {
    idt[n].base_lo = (uint16_t)(h & 0xFFFF);
    idt[n].sel     = 0x08;          // kernel code selector from your GDT
    idt[n].zero    = 0;
    idt[n].flags   = 0x8E;          // present, ring0, 32-bit interrupt gate
    idt[n].base_hi = (uint16_t)((h >> 16) & 0xFFFF);
}

void idt_init(void) {
    for (int i = 0; i < 256; i++) set_gate(i, (uint32_t)isr_stub);
    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uint32_t)&idt[0];
    set_gate(32, (uint32_t)irq0_stub);
    set_gate(33, (uint32_t)irq1_stub);
    idt_load(&idtr);
}

