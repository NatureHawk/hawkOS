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
