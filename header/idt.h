#pragma once


#include <stdint.h>

void idt_init(void);
void set_gate(int i, uint32_t handler);

// Same, but with an explicit caller privilege level. DPL 3 is what lets a
// ring-3 program reach a gate at all; everything else stays at 0.
void set_gate_dpl(int i, uint32_t handler, int dpl);
