
#pragma once
#include <stdint.h>

// goes up every timer interrupt (IRQ0)
extern volatile unsigned long long ticks;


void pit_init(uint32_t hz);
