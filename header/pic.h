
#pragma once
#include <stdint.h>

void pic_remap(uint8_t offset1, uint8_t offset2);
void pic_set_mask(uint8_t irq);    // keep masked = disabled
void pic_clear_mask(uint8_t irq);  // unmask = enable
void pic_send_eoi(uint8_t irq);    // we'll use later in IRQ handlers
