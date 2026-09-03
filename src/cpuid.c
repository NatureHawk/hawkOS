// src/cpuid.c
#include <stdint.h>
#include "header/cpuid.h"

static inline void do_cpuid(uint32_t leaf, uint32_t subleaf,
                             uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d){
    __asm__ __volatile__("cpuid"
        : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
        : "a"(leaf), "c"(subleaf));
}

// Writes little-endian bytes of v — avoids aliasing a char* through a
// uint32_t*, which would be undefined behaviour at -O2.
static inline void put_u32(char* dst, uint32_t v){
    dst[0] = (char)(v & 0xFF);
    dst[1] = (char)((v >> 8) & 0xFF);
    dst[2] = (char)((v >> 16) & 0xFF);
    dst[3] = (char)((v >> 24) & 0xFF);
}

void cpuid_vendor(char out[13]){
    uint32_t a, b, c, d;
    do_cpuid(0, 0, &a, &b, &c, &d);
    // CPUID leaf 0 returns the 12-char vendor string in EBX:EDX:ECX order.
    put_u32(out + 0, b);
    put_u32(out + 4, d);
    put_u32(out + 8, c);
    out[12] = 0;
}

void cpuid_brand(char out[49]){
    uint32_t a, b, c, d;
    do_cpuid(0x80000000u, 0, &a, &b, &c, &d);
    if (a < 0x80000004u){
        const char* na = "(brand string not supported on this CPU)";
        int i = 0;
        while (na[i] && i < 48){ out[i] = na[i]; i++; }
        out[i] = 0;
        return;
    }
    do_cpuid(0x80000002u, 0, &a, &b, &c, &d);
    put_u32(out + 0, a); put_u32(out + 4, b); put_u32(out + 8, c); put_u32(out + 12, d);
    do_cpuid(0x80000003u, 0, &a, &b, &c, &d);
    put_u32(out + 16, a); put_u32(out + 20, b); put_u32(out + 24, c); put_u32(out + 28, d);
    do_cpuid(0x80000004u, 0, &a, &b, &c, &d);
    put_u32(out + 32, a); put_u32(out + 36, b); put_u32(out + 40, c); put_u32(out + 44, d);
    out[48] = 0;
}
