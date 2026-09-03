#include <stdint.h>
#include "header/kprintf.h"
#include "header/kbd.h"
#include "header/tty.h"

static inline void slp(void){ __asm__ __volatile__("hlt"); }

int tty_readline(const char *p, char *b, int n){
    int i=0;
    if(p) kprintf("%s", p);
    for(;;){
        int c = kbd_getc();
        if(c<0){ slp(); continue; }
        if(c=='\r' || c=='\n'){
            kprintf("\n");
            if(i<n) b[i]=0;
            return i;
        }
        if(c=='\b'){
            if(i>0){ i--; kprintf("\b \b"); }
            continue;
        }
        if(c=='\t') c=' ';
        if(c>=32 && c<=126){
            if(i<n-1){ b[i++]=(char)c; kprintf("%c", c); }
        }
    }
}
