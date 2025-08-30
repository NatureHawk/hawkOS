#include "header/pit.h"
#include "header/io.h"

extern volatile unsigned long long ticks;

void irq0_handler_c(void){
    ticks++;            // bump global tick counter
    outb(0x20, 0x20);   // EOI to master PIC
}