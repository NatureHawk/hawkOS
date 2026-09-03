#include "header/pit.h"
#include "header/io.h"
#include "header/task.h"

extern volatile unsigned long long ticks;

void irq0_handler_c(void){
    ticks++;            // bump global tick counter
    outb(0x20, 0x20);   // EOI to master PIC

    // EOI first, then schedule. If we switched stacks before acknowledging
    // the PIC, the outgoing task would carry the un-acked interrupt with it
    // and the timer would never fire again.
    sched_tick();
}
