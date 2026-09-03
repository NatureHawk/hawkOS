// src/vmmouse.c — VMware absolute pointer, as emulated by QEMU
//
// The whole protocol is one instruction: an `inl` from port 0x5658 with a
// magic value in EAX, a command in ECX and a parameter in EBX. The device
// answers in all four registers. There is no memory mapping, no descriptor
// ring and no interrupt of its own — which is what makes absolute pointing
// affordable here, where a USB tablet would have meant writing a USB stack
// first.
#include <stdint.h>
#include "header/vmmouse.h"
#include "header/kprintf.h"

#define VMWARE_MAGIC 0x564D5868u      // "VMXh"
#define VMWARE_PORT  0x5658

#define CMD_GETVERSION         10
#define CMD_ABSPOINTER_DATA    39
#define CMD_ABSPOINTER_STATUS  40
#define CMD_ABSPOINTER_COMMAND 41

#define ABSPOINTER_ENABLE      0x45414552u
#define ABSPOINTER_RELATIVE    0x4C455252u
#define ABSPOINTER_ABSOLUTE    0x53424152u
#define ABSPOINTER_OFF         0x000000F5u

// The word ENABLE leaves in the queue, echoed back to prove the device is
// really there and really speaking this protocol.
#define VMMOUSE_VERSION        0x3442554Au

static int active = 0;

// EBX is a general register here, not a PIC base pointer: the kernel is
// built with -fno-pic, so constraining it is safe.
static inline void vm_cmd(uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d){
    __asm__ __volatile__("inl %%dx, %%eax"
                         : "+a"(*a), "+b"(*b), "+c"(*c), "+d"(*d)
                         :
                         : "memory");
}

int vmmouse_detect(void){
    uint32_t a = VMWARE_MAGIC, b = ~VMWARE_MAGIC, c = CMD_GETVERSION, d = VMWARE_PORT;
    vm_cmd(&a, &b, &c, &d);

    // The port echoes the magic back in EBX when something is listening. On
    // a machine with no vmport the `in` reads open bus and EBX is unchanged.
    return (b == VMWARE_MAGIC && a != 0xFFFFFFFFu);
}

int vmmouse_enable(void){
    active = 0;
    if (!vmmouse_detect()) return -1;

    uint32_t a = VMWARE_MAGIC, b = ABSPOINTER_ENABLE, c = CMD_ABSPOINTER_COMMAND, d = VMWARE_PORT;
    vm_cmd(&a, &b, &c, &d);

    a = VMWARE_MAGIC; b = 0; c = CMD_ABSPOINTER_STATUS; d = VMWARE_PORT;
    vm_cmd(&a, &b, &c, &d);
    if ((a & 0xFFFF0000u) == 0xFFFF0000u) return -1;

    // Enabling queues exactly ONE word: a version stamp. Asking for four
    // here (the size of a real data packet) reads past the end of the queue
    // and leaves it out of step, after which the device accepts the absolute
    // request but never delivers a single event.
    a = VMWARE_MAGIC; b = 1; c = CMD_ABSPOINTER_DATA; d = VMWARE_PORT;
    vm_cmd(&a, &b, &c, &d);
    if (a != VMMOUSE_VERSION){
        kprintf("[vmmouse] version handshake returned %x, expected %x\n",
                a, VMMOUSE_VERSION);
        return -1;
    }

    a = VMWARE_MAGIC; b = ABSPOINTER_ABSOLUTE; c = CMD_ABSPOINTER_COMMAND; d = VMWARE_PORT;
    vm_cmd(&a, &b, &c, &d);

    active = 1;
    return 0;
}

void vmmouse_disable(void){
    if (!active) return;
    uint32_t a = VMWARE_MAGIC, b = ABSPOINTER_OFF, c = CMD_ABSPOINTER_COMMAND, d = VMWARE_PORT;
    vm_cmd(&a, &b, &c, &d);
    active = 0;
}

int vmmouse_poll(uint32_t* x, uint32_t* y, int* buttons, int* wheel){
    if (!active) return -1;

    uint32_t a = VMWARE_MAGIC, b = 0, c = CMD_ABSPOINTER_STATUS, d = VMWARE_PORT;
    vm_cmd(&a, &b, &c, &d);

    if ((a & 0xFFFF0000u) == 0xFFFF0000u){ active = 0; return -1; }

    uint16_t words = (uint16_t)(a & 0xFFFFu);
    if (words == 0) return 0;
    if (words % 4){ active = 0; return -1; }      // desynchronised queue

    a = VMWARE_MAGIC; b = 4; c = CMD_ABSPOINTER_DATA; d = VMWARE_PORT;
    vm_cmd(&a, &b, &c, &d);

    // a: flags in the high half, button bits in the low half
    // b: absolute x, c: absolute y, both 0..65535
    // d: wheel delta, as a signed byte
    if (buttons) *buttons = (int)(a & 0xFFFFu);
    if (x)       *x = b;
    if (y)       *y = c;
    if (wheel)   *wheel = (int)(int8_t)(d & 0xFFu);
    return 1;
}
