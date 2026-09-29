
#pragma once
#include <stdint.h>

// goes up every timer interrupt (IRQ0)
extern volatile unsigned long long ticks;


void pit_init(uint32_t hz);

// Busy-wait up to 50000 us on PIT channel 2 (channel 0 and `ticks` untouched).
void pit_wait_us(uint32_t us);
