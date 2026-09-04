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
extern void irq12_stub(void);

void set_gate_dpl(int n, uint32_t h, int dpl) {
    idt[n].base_lo = (uint16_t)(h & 0xFFFF);
    idt[n].sel     = 0x08;          // kernel code selector from your GDT
    idt[n].zero    = 0;
    // Present, 32-bit interrupt gate, with the caller privilege the gate will
    // accept. Only the system call vector is opened to ring 3; leaving any
    // other gate at DPL 3 would let a user program invoke a handler that
    // expects to have been entered by hardware.
    idt[n].flags   = (uint8_t)(0x8E | ((dpl & 3) << 5));
    idt[n].base_hi = (uint16_t)((h >> 16) & 0xFFFF);
}

void set_gate(int n, uint32_t h) { set_gate_dpl(n, h, 0); }

// Exposed for the self-test, which asserts that exactly one gate is reachable
// from ring 3. Reading the table directly is the only way to check that;
// anything else tests what the code meant rather than what it did.
void* idt_base_for_test(void){ return (void*)&idt[0]; }

void idt_init(void) {
    for (int i = 0; i < 256; i++) set_gate(i, (uint32_t)isr_stub);
    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uint32_t)&idt[0];
    set_gate(32, (uint32_t)irq0_stub);
    set_gate(33, (uint32_t)irq1_stub);
    set_gate(44, (uint32_t)irq12_stub);   // IRQ12 = PS/2 mouse (slave PIC line 4)
    idt_load(&idtr);
}

