#include <stdarg.h>
#include <stdint.h>
#include "console.h"
#include "serial.h"
#include "kprintf.h"

static void outc(char c){
    console_putc(c);
    serial_putc(c);
}

static void outs(const char *s){
    while(*s) outc(*s++);
}

static void out_udec(uint32_t v){
    char b[10]; int i=0;
    if(v==0){ outc('0'); return; }
    while(v){ b[i++] = '0' + (v%10); v/=10; }
    while(i--) outc(b[i]);
}

static void out_dec(int32_t v){
    if(v<0){ outc('-'); out_udec((uint32_t)(-v)); }
    else out_udec((uint32_t)v);
}

static void out_hex(uint32_t v, int pref){
    if(pref){ outs("0x"); }
    for(int i=7;i>=0;i--){
        uint8_t n=(v>>(i*4))&0xF;
        outc(n<10? '0'+n : 'A'+(n-10));
    }
}

int kprintf(const char *fmt, ...){
    va_list ap; va_start(ap, fmt);
    int cnt=0;
    for(; *fmt; fmt++){
        if(*fmt!='%'){ outc(*fmt); cnt++; continue; }
        fmt++;
        if(*fmt=='%'){ outc('%'); cnt++; continue; }
        switch(*fmt){
            case 'c': { char c=(char)va_arg(ap,int); outc(c); cnt++; } break;
            case 's': { const char* s=va_arg(ap,const char*); if(!s)s="(null)"; outs(s); while(*s++) cnt++; } break;
            case 'd': { int32_t v=va_arg(ap,int32_t); out_dec(v); } break;
            case 'u': { uint32_t v=va_arg(ap,uint32_t); out_udec(v); } break;
            case 'x': { uint32_t v=va_arg(ap,uint32_t); out_hex(v,0); } break;
            case 'p': { uint32_t v=(uint32_t)va_arg(ap,void*); out_hex(v,1); } break;
            default: outc('%'); outc(*fmt); cnt+=2; break;
        }
    }
    va_end(ap);
    return cnt;
}
