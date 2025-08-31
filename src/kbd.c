// src/kbd.c
#include "header/io.h"
#include "header/kbd.h"

// --- simple ring buffer for chars typed ---
#define KBD_BUF_SIZE 128
static volatile int head = 0, tail = 0;
static char buf[KBD_BUF_SIZE];

static inline void push(char c){
    int nxt = (head + 1) % KBD_BUF_SIZE;
    if (nxt != tail) { buf[head] = c; head = nxt; } // drop if full
}

int kbd_getc(void){
    if (tail == head) return -1;
    char c = buf[tail];
    tail = (tail + 1) % KBD_BUF_SIZE;
    return (int)(unsigned char)c;
}

void kbd_init(void){ /* nothing yet */ }

// --- scancode tables (set 1), only the basics we care about ---
static const char normal[128] = {
/*0x00*/ 0,   27,  '1','2','3','4','5','6','7','8','9','0','-','=', '\b', /*0x0F*/ '\t',
/*0x10*/ 'q','w','e','r','t','y','u','i','o','p','[',']','\n', 0,  'a','s',
/*0x20*/ 'd','f','g','h','j','k','l',';','\'','`',  0, '\\','z','x','c','v',
/*0x30*/ 'b','n','m',',','.','/',  0,  '*',  0,  ' ', 0,   0,   0,   0,   0,   0,
/*0x40*/ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
/*0x50*/ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
/*0x60*/ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
/*0x70*/ 0,0,0,0,0,0,0,0
};

static const char shifted[128] = {
/*0x00*/ 0,   27,  '!','@','#','$','%','^','&','*','(',')','_','+', '\b', /*0x0F*/ '\t',
/*0x10*/ 'Q','W','E','R','T','Y','U','I','O','P','{','}','\n', 0,  'A','S',
/*0x20*/ 'D','F','G','H','J','K','L',':','"','~',  0,  '|','Z','X','C','V',
/*0x30*/ 'B','N','M','<','>','?',  0,  '*',  0,  ' ', 0,   0,   0,   0,   0,   0,
/*0x40*/ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
/*0x50*/ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
/*0x60*/ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
/*0x70*/ 0,0,0,0,0,0,0,0
};

static int shift_on = 0;

// called from IRQ1 (keyboard) C handler
static void handle_scancode(uint8_t sc){
    // key release? (high bit set)
    if (sc & 0x80){
        uint8_t code = sc & 0x7F;
        if (code == 0x2A || code == 0x36) shift_on = 0; // LShift or RShift release
        return;
    }
    // key press
    if (sc == 0x2A || sc == 0x36) { shift_on = 1; return; } // Shift down

    char ch = shift_on ? shifted[sc] : normal[sc];
    if (!ch) return;                  // ignore keys we didn't map

    // Backspace handling: just push '\b' and let caller decide how to render
    push(ch);
}

// exported C function the ASM stub calls
void keyboard_handler_c(void){
    uint8_t sc = inb(0x60);   // read scancode
    handle_scancode(sc);
    outb(0x20, 0x20);         // EOI to master PIC
}
