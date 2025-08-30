#include <stdint.h>
#include "header/pit.h"
#include "header/io.h"  


static volatile uint16_t* const vga = (uint16_t*)0xB8000;
static int row = 0, col = 0;
static uint8_t attr = 0x0F;

static inline void putc(char ch) {
    if (ch == '\n') { col = 0; row++; return; }
    vga[row*80 + col] = ((uint16_t)attr << 8) | (uint8_t)ch;
    if (++col >= 80) { col = 0; row++; }
}

static void puts(const char* s) { while (*s) putc(*s++); }

void kernel_main(void) {
    for (int i = 0; i < 80*25; i++)
        vga[i] = ((uint16_t)0x07 << 8) | ' ';
    row = col = 0; attr = 0x0F;

    puts("HawkOS: hybrid boot (ASM) + kernel (C)\n");
    puts("We are in 32-bit protected mode.\n");

    for (;;) __asm__ __volatile__("hlt");
}
