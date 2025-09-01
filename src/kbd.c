// src/kbd.c
#include "header/io.h"
#include "header/kbd.h"

#define KBD_BUF_SIZE 128

static volatile int head = 0, tail = 0;
static char buf[KBD_BUF_SIZE];

static volatile int shift_on = 0;
static volatile int caps_on  = 0;

static inline void push(char c){
    int n = (head + 1) % KBD_BUF_SIZE;
    if (n != tail) { buf[head] = c; head = n; }
}

int kbd_getc(void){
    if (tail == head) return -1;
    char c = buf[tail];
    tail = (tail + 1) % KBD_BUF_SIZE;
    return (int)(unsigned char)c;
}

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

static void handle_scancode(uint8_t sc){
    if (sc & 0x80){
        uint8_t code = sc & 0x7F;
        if (code == 0x2A || code == 0x36) shift_on = 0;
        return;
    }

    if (sc == 0x2A || sc == 0x36){ shift_on = 1; return; }
    if (sc == 0x3A){ caps_on ^= 1; return; }

    char base = normal[sc];
    if (!base) return;

    char ch = 0;
    if (base >= 'a' && base <= 'z'){
        int upper = (shift_on ^ caps_on);
        ch = upper ? (char)('A' + (base - 'a')) : base;
    }else{
        ch = shift_on ? shifted[sc] : base;
    }

    if (ch) push(ch);
}

void keyboard_handler_c(void){
    uint8_t sc = inb(0x60);
    handle_scancode(sc);
    outb(0x20, 0x20);
}
