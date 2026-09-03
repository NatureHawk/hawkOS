// src/kbd.c — PS/2 keyboard (IRQ1), scancode set 1
#include "header/io.h"
#include "header/kbd.h"

#define KBD_BUF_SIZE 128

// The ring buffer holds ints, not chars: arrow and navigation keys have no
// ASCII value, so they are delivered as KEY_* codes above 0xFF through the
// same queue rather than through a second side channel.
static volatile int head = 0, tail = 0;
static int buf[KBD_BUF_SIZE];

static volatile int shift_on = 0;
static volatile int caps_on  = 0;
static volatile int ctrl_on  = 0;
static volatile int e0_seen  = 0;

static inline void push(int c){
    int n = (head + 1) % KBD_BUF_SIZE;
    if (n != tail) { buf[head] = c; head = n; }
}

int kbd_getc(void){
    if (tail == head) return -1;
    int c = buf[tail];
    tail = (tail + 1) % KBD_BUF_SIZE;
    return c;
}

int kbd_ctrl_down(void){ return ctrl_on; }
int kbd_shift_down(void){ return shift_on; }

void kbd_init(void){ }

static const char normal[128] = {
/*00*/ 0,27,'1','2','3','4','5','6','7','8','9','0','-','=', '\b','\t',
/*10*/ 'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,'a','s',
/*20*/ 'd','f','g','h','j','k','l',';','\'','`',0,'\\','z','x','c','v',
/*30*/ 'b','n','m',',','.','/',0,'*',0,' ',0,0,0,0,0,0,
/*40*/ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
/*50*/ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
/*60*/ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
/*70*/ 0,0,0,0,0,0,0,0
};

static const char shifted[128] = {
/*00*/ 0,27,'!','@','#','$','%','^','&','*','(',')','_','+','\b','\t',
/*10*/ 'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,'A','S',
/*20*/ 'D','F','G','H','J','K','L',':','"','~',0,'|','Z','X','C','V',
/*30*/ 'B','N','M','<','>','?',0,'*',0,' ',0,0,0,0,0,0,
/*40*/ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
/*50*/ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
/*60*/ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
/*70*/ 0,0,0,0,0,0,0,0
};

// Navigation keys arrive as 0xE0 followed by the same scancode the numeric
// keypad uses; the prefix is the only thing distinguishing them.
static int extended_key(uint8_t code){
    switch (code) {
        case 0x48: return KEY_UP;
        case 0x50: return KEY_DOWN;
        case 0x4B: return KEY_LEFT;
        case 0x4D: return KEY_RIGHT;
        case 0x49: return KEY_PGUP;
        case 0x51: return KEY_PGDN;
        case 0x47: return KEY_HOME;
        case 0x4F: return KEY_END;
        case 0x53: return KEY_DELETE;
        default:   return 0;
    }
}

static void handle_scancode(uint8_t sc){
    if (sc == 0xE0){ e0_seen = 1; return; }

    if (sc & 0x80){                       // key release
        uint8_t code = sc & 0x7F;
        if (e0_seen){ e0_seen = 0; if (code == 0x1D) ctrl_on = 0; return; }
        if (code == 0x2A || code == 0x36) shift_on = 0;
        if (code == 0x1D) ctrl_on = 0;
        return;
    }

    if (e0_seen){
        e0_seen = 0;
        if (sc == 0x1D){ ctrl_on = 1; return; }
        int k = extended_key(sc);
        if (k) push(k);
        return;
    }

    if (sc == 0x2A || sc == 0x36){ shift_on = 1; return; }
    if (sc == 0x1D){ ctrl_on = 1; return; }
    if (sc == 0x3A){ caps_on ^= 1; return; }
    if (sc == 0x3B){ push(KEY_F1); return; }

    // The keypad arrows send these without an 0xE0 prefix when Num Lock is
    // off, which is how QEMU's default keymap delivers them.
    if (!normal[sc]) {
        int k = extended_key(sc);
        if (k) push(k);
        return;
    }

    char base = normal[sc];
    char ch;
    if (base >= 'a' && base <= 'z'){
        int upper = (shift_on ^ caps_on);
        ch = upper ? (char)('A' + (base - 'a')) : base;
    }else{
        ch = shift_on ? shifted[sc] : base;
    }

    if (ch) push((int)(unsigned char)ch);
}

void keyboard_handler_c(void){
    uint8_t sc = inb(0x60);
    handle_scancode(sc);
    outb(0x20, 0x20);
}
