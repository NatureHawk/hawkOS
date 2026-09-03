#include <stdint.h>
#include "header/pit.h"
#include "header/io.h"
#include "header/idt.h"
#include "header/pic.h"
#include "header/kbd.h"
#include "header/console.h"
#include "header/exceptions.h"
#include "header/pmm.h"
#include "header/paging.h"
#include "header/kheap.h"
#include "header/gfx.h"
#include "header/mouse.h"
#include "header/shell.h"
#include "header/desktop.h"
#include "header/net.h"
#include "header/netcfg.h"
#include "header/task.h"
#include "serial.h"
#include "kprintf.h"

// DHCP takes a few hundred milliseconds at best and can time out entirely,
// so it runs on its own task: the desktop comes up immediately and the
// network configures itself behind it.
static void netcfg_task(void* arg){
    (void)arg;
    netcfg_init(8000);
}

void kernel_main(uint32_t mb_magic, uint32_t mb_info) {
    serial_init();

    pmm_init(mb_magic, mb_info);
    paging_init();
    gfx_init(mb_magic, mb_info);   // needs paging_init done first (identity-maps the fb)
    console_init();                // needs gfx sized, so it must come after gfx_init
    kheap_init();

    kprintf("HawkOS: hybrid boot (ASM) + kernel (C)\n");
    kprintf("We are in 32-bit protected mode.\n");

    idt_init();
    exceptions_init();
    kprintf("\n[boot] serial online, %s\n", gfx_available() ? "framebuffer online" : "no framebuffer (text lost, serial still live)");
    kprintf("[boot] tick=%u (start)\n", 0u);

    void* test = kmalloc(64);
    kprintf("[kheap] test alloc -> %p\n", test);
    kfree(test);

    pic_remap(0x20, 0x28);

    // IRQ0 (PIT), IRQ1 (keyboard), IRQ2 (cascade, required for any slave-PIC
    // line to reach the CPU), IRQ12 (mouse, slave line 4) all unmasked.
    outb(0x21, 0xF8);   // 11111000: bits 0,1,2 clear
    outb(0xA1, 0xEF);   // 11101111: bit 4 clear

    pit_init(100);
    kbd_init();
    mouse_init();

    // Adopt this boot context as task 0 before enabling interrupts, so the
    // very first timer tick already has a valid task table to schedule from.
    task_init();

    __asm__ __volatile__("sti");

    net_init();
    if (net_link_up()) task_create("netcfg", netcfg_task, 0);

    // With a framebuffer the desktop is the system: the shell lives inside
    // it as the Terminal app, so there is no reason to sit at a text prompt
    // first. Without one, fall back to the text shell on the serial console.
    if (gfx_available()) desktop_run();
    else                 shell_run();

    for(;;) __asm__ __volatile__("hlt");
}
