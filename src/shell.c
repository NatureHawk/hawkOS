#include <stdint.h>
#include "header/kprintf.h"
#include "header/tty.h"

extern volatile unsigned long long ticks;

static int streq(const char* a,const char* b){
    while(*a||*b){ if(*a!=*b) return 0; a++; b++; } return 1;
}
static int starts(const char* s,const char* p){
    while(*p){ if(*s++!=*p++) return 0; } return 1;
}

void shell_run(void){
    char b[128];
    kprintf("[shell] type 'help'\n");
    for(;;){
        int n = tty_readline("> ", b, sizeof(b));
        if(n<=0) continue;

        if(streq(b,"help")){
            kprintf("help | echo <text> | uptime\n");
        }else if(starts(b,"echo ")){
            kprintf("%s\n", b+5);
        }else if(streq(b,"uptime")){
            unsigned long long t = ticks;
            unsigned s = (unsigned)(t/100);
            unsigned ms = (unsigned)(t%100)*10;
            kprintf("%us %ums\n", s, ms);
        }else{
            kprintf("unknown: %s\n", b);
        }
    }
}
