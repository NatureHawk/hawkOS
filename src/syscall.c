// src/syscall.c — the system call dispatcher
//
// Every argument arriving here came from a program the kernel does not trust,
// so every pointer is checked before it is dereferenced. That check is the
// entire point of the privilege boundary: without it, ring 3 is a formality
// and a user program can still make the kernel read or write anywhere by
// passing a kernel address to write().
#include <stdint.h>
#include "header/syscall.h"
#include "header/idt.h"
#include "header/task.h"
#include "header/proc.h"
#include "header/kprintf.h"
#include "header/kstring.h"

extern volatile unsigned long long ticks;
extern void syscall_stub(void);

static uint32_t calls = 0;

uint32_t syscall_count(void){ return calls; }

void syscall_init(void){
    // DPL 3 so ring 3 may invoke it; every other vector stays ring 0.
    set_gate_dpl(0x80, (uint32_t)syscall_stub, 3);
    kprintf("[syscall] int 0x80 gate installed (%d calls)\n", SYS_MAX - 1);
}

// A write of more than this is refused rather than truncated: it is far more
// likely to be a wild length than a genuine request.
#define WRITE_MAX 4096

void syscall_dispatch(syscall_regs_t* r){
    calls++;

    uint32_t nr = r->eax;
    uint32_t a1 = r->ebx, a2 = r->ecx, a3 = r->edx;

    switch (nr){
        case SYS_EXIT:
            proc_exit((int)a1);
            r->eax = 0;
            break;

        case SYS_WRITE: {
            // a1 = buffer, a2 = length. The buffer has to lie entirely inside
            // the calling process's own image, which proc_valid_range checks
            // against the mapping it was given -- not against "is it mapped",
            // which would happily accept a kernel address.
            if (a2 > WRITE_MAX){ r->eax = (uint32_t)-1; break; }
            if (!proc_valid_range(a1, a2)){ r->eax = (uint32_t)-1; break; }
            const char* s = (const char*)a1;
            for (uint32_t i = 0; i < a2; i++) kprintf("%c", s[i]);
            r->eax = a2;
        } break;

        case SYS_GETPID:
            r->eax = task_current_id();
            break;

        case SYS_YIELD:
            task_yield();
            r->eax = 0;
            break;

        case SYS_SLEEP:
            if (a1 > 60000u) a1 = 60000u;
            task_sleep(a1);
            r->eax = 0;
            break;

        case SYS_TICKS:
            r->eax = (uint32_t)ticks;
            break;

        case SYS_OPEN: {
            // a1 = NUL-terminated name inside the process image. Returns the
            // file's size, which is all a program needs before calling read.
            if (!proc_valid_string(a1)){ r->eax = (uint32_t)-1; break; }
            r->eax = (uint32_t)proc_open((const char*)a1);
        } break;

        case SYS_READ: {
            // a1 = name, a2 = buffer, a3 = capacity.
            if (!proc_valid_string(a1) || !proc_valid_range(a2, a3)){
                r->eax = (uint32_t)-1; break;
            }
            r->eax = proc_read((const char*)a1, (uint8_t*)a2, a3);
        } break;

        default:
            kprintf("[syscall] pid %u made unknown call %u\n", task_current_id(), nr);
            r->eax = (uint32_t)-1;
            break;
    }
}
