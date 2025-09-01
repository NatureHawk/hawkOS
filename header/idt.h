#pragma once


void idt_init(void);
void set_gate(int i, uint32_t handler);
