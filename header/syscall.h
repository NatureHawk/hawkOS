#pragma once
#include <stdint.h>

// The system call interface.
//
// int 0x80 with the call number in eax and arguments in ebx, ecx, edx. The
// result comes back in eax. This is the Linux i386 convention, chosen because
// it is the one every reference and every reader already knows -- there is
// nothing to gain from inventing a different register order.
//
// The gate at 0x80 is the only IDT entry with DPL 3. Every other vector is
// ring 0, so a user program that tries to invoke one gets a general
// protection fault rather than a kernel entry point.

#define SYS_EXIT     1
#define SYS_WRITE    2
#define SYS_GETPID   3
#define SYS_YIELD    4
#define SYS_SLEEP    5
#define SYS_TICKS    6
#define SYS_OPEN     7   // returns a file size, or -1; see proc.c
#define SYS_READ     8
#define SYS_MAX      9

// Pushed by syscall_stub. The field order is the reverse of the pushes, so
// this maps exactly onto what pusha and the segment saves leave on the stack.
typedef struct {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t eip, cs, eflags, useresp, ss;
} syscall_regs_t;

void syscall_init(void);
void syscall_dispatch(syscall_regs_t* r);

// Number of system calls serviced since boot, for the task manager.
uint32_t syscall_count(void);
