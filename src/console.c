#include "console.h"

static volatile uint16_t* const vga = (uint16_t*)0xB8000;
static int row = 0, col = 0;
static uint8_t attr = 0x0F;

static inline void put_cell(int x,int y,char ch,uint8_t a){
    vga[y*80 + x] = ((uint16_t)a << 8) | (uint8_t)ch;
}

void console_init(void){
    for (int i = 0; i < 80*25; i++) vga[i] = ((uint16_t)0x07 << 8) | ' ';
    row = col = 0; attr = 0x0F;
}

static void newline(void){
    col = 0; row++;
    if (row >= 25) { // simple scrollingk up by 1 line
        for (int y=1; y<25; y++)
            for (int x=0; x<80; x++)
                vga[(y-1)*80 + x] = vga[y*80 + x];
        for (int x=0; x<80; x++) put_cell(x, 24, ' ', attr);
        row = 24;
    }
}

void console_putc(char ch){
    if (ch == '\n') { newline(); return; }
    if (ch == '\r') { col = 0; return; }
    if (ch == '\b' || ch == 0x7F) {            // backspace 
        if (col > 0) {
            col--;
            put_cell(col, row, ' ', attr);     // erase
        } else if (row > 0) {
            row--; col = 79;
            put_cell(col, row, ' ', attr);
        }
        return;
    }
    put_cell(col, row, ch, attr);
    if (++col >= 80) newline();
}

void console_puts(const char* s){ while (*s) console_putc(*s++); }

void console_printu_at(int x,int y,unsigned long long n){
    char b[21]; int i=0;
    if (!n) { put_cell(x,y,'0',attr); return; }
    while (n){ b[i++] = '0' + (n%10); n/=10; }
    for (int k=0;k<i;k++) put_cell(x+k,y,b[i-1-k],attr);
}
