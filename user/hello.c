// user/hello.c — a program that runs in ring 3
//
// Nothing here is linked against the kernel. It cannot call kprintf, it cannot
// touch a device, and it cannot read kernel memory: the only way it affects
// anything outside its own two mappings is int 0x80. That is the whole point
// of it existing.
//
// Built as a flat binary (the ELF programs are hi, cat, ls, utest) linked at PROC_BASE (see user/user.ld) and copied
// onto the disk image, where proc_spawn finds it by name.

#include "hawk.h"

static void print(const char* s){ hawk_puts(s); }
static void print_num(unsigned v){ hawk_putnum(v); }
static int syscall1(int nr, int a){ return hawk_sys3(nr, a, 0, 0); }
static int syscall0(int nr){ return hawk_sys3(nr, 0, 0, 0); }

// The entry point has to be the first byte of the image: proc_spawn jumps
// straight to PROC_BASE, with no header to say otherwise. user.ld places this
// section first to guarantee it.
__attribute__((section(".text.entry"), used))
void _start(void){
    print("hello from ring 3\n");

    print("  pid   = ");
    print_num((unsigned)syscall0(SYS_GETPID));
    print("\n  ticks = ");
    print_num((unsigned)syscall0(SYS_TICKS));
    print("\n");

    // Prove the process is really preemptible and really scheduled alongside
    // the kernel's own tasks, rather than running to completion in one slice.
    for (int i = 0; i < 3; i++){
        syscall1(SYS_SLEEP, 60);
        print("  tick ");
        print_num((unsigned)i);
        print("\n");
    }

    // And prove the boundary holds. Reading kernel memory from ring 3 has to
    // fault; if this ever returns, the isolation is not real. It is the last
    // thing the program does so that everything above still gets reported.
    print("attempting to read kernel memory at 0x100000...\n");
    volatile unsigned* kernel = (volatile unsigned*)0x100000;
    unsigned stolen = *kernel;
    print("BUG: read succeeded, value = ");
    print_num(stolen);
    print("\n");

    syscall1(SYS_EXIT, 0);
    for(;;){ }
}
