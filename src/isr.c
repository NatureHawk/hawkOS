// src/isr.c
#include "header/pit.h"
#include "header/io.h"

// called from the asm stub for IRQ0
void irq0_handler_c(void){
    ticks++;            // 1) bump ticks
    outb(0x20, 0x20);   // 2) EOI to master PIC
}
