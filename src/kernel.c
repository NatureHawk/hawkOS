#include <stdint.h>
#include "header/pit.h"
#include "header/io.h"  
#include "header/idt.h"
#include "header/pic.h"
#include "header/kbd.h"
#include "header/console.h"
#include "header/exceptions.h"
#include "serial.h"
#include "kprintf.h"



static volatile uint16_t* const vga = (uint16_t*)0xB8000;
static int row = 0, col = 0;
static uint8_t attr = 0x0F;

static inline void putc(char ch) {
    if (ch == '\n') { col = 0; row++; return; }
    vga[row*80 + col] = ((uint16_t)attr << 8) | (uint8_t)ch;
    if (++col >= 80) { col = 0; row++; }
}

static void printu_at(int x, int y, unsigned long long n){
    char b[21]; int i = 0;
    if (!n) { vga[y*80 + x] = ((uint16_t)attr << 8) | '0'; return; }
    while (n) { b[i++] = '0' + (n % 10); n /= 10; }
    for (int k = 0; k < i; k++)
        vga[y*80 + x + k] = ((uint16_t)attr << 8) | b[i-1-k];
}


static void puts(const char* s) { while (*s) putc(*s++); }

void kernel_main(void) {
    console_init();

    console_puts("HawkOS: hybrid boot (ASM) + kernel (C)\n");
    console_puts("We are in 32-bit protected mode.\n");

    idt_init();
    exceptions_init();
    serial_init();
    kprintf("\n[boot] serial online, vga online\n");
    kprintf("[boot] tick=%u (start)\n", 0u);
    
    pic_remap(0x20,0x28);

    outb(0x21, 0xFC); //enabling the timer and keyboard intterupts
    outb(0xA1, 0xFF);
    

    pit_init(100);
    kbd_init();
    __asm__ __volatile__("sti");
    
    console_puts("tick-\n");
    for (;;) {
        printu_at(5,2, ticks);

        int ch=kbd_getc();
        if(ch !=-1)console_putc((char)ch);
        __asm__ __volatile__("hlt");
    }
    
}
