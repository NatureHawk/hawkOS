#include <stdint.h>
#include "header/io.h"   // outb/inb
#include "header/pit.h"

volatile unsigned long long ticks = 0;

void pit_init(uint32_t hz){
    if(hz == 0) hz = 100;            // default
    uint32_t div = 1193182u / hz;    // PIT base clock ~1.193182 MHz
    // channel 0, lobyte/hibyte, mode 3 (square wave), binary
    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(div & 0xFF));
    outb(0x40, (uint8_t)((div >> 8) & 0xFF));
}

// Busy-waits using channel 2, which is otherwise idle, so the 100 Hz channel-0
// tick is never reprogrammed or disturbed. Mode 0 (interrupt on terminal
// count): OUT2 goes high, readable at port 0x61 bit 5, once the count expires.
// A 16-bit count caps a single wait at ~54 ms. Used to calibrate the LAPIC
// timer, which has no other reference clock.
void pit_wait_us(uint32_t us){
    if (us > 50000u) us = 50000u;
    uint32_t count = (uint32_t)(((unsigned long long)us * 1193182ull) / 1000000ull);
    if (count == 0) count = 1;

    outb(0x61, (uint8_t)((inb(0x61) & 0xFC) | 0x01));    // gate on, speaker off
    outb(0x43, 0xB0);                                     // ch2, lobyte/hibyte, mode 0
    outb(0x42, (uint8_t)(count & 0xFF));
    outb(0x42, (uint8_t)(count >> 8));
    for (uint32_t i = 0; i < 100000000u; i++)
        if (inb(0x61) & 0x20) break;
}
