#include "header/io.h"
#include "header/pic.h"

#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1

#define ICW1_INIT  0x10
#define ICW1_ICW4  0x01
#define ICW4_8086  0x01

void pic_remap(uint8_t offset1, uint8_t offset2){
    uint8_t a1 = inb(PIC1_DATA);
    uint8_t a2 = inb(PIC2_DATA);

    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4); io_wait();

    outb(PIC1_DATA, offset1); io_wait();
    outb(PIC2_DATA, offset2); io_wait();

    outb(PIC1_DATA, 0x04); io_wait();
    outb(PIC2_DATA, 0x02); io_wait();

    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    outb(PIC1_DATA, a1);
    outb(PIC2_DATA, a2);
}

void pic_set_mask(uint8_t irq){
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t  line = (irq < 8) ? irq : irq - 8;
    uint8_t  mask = inb(port);
    outb(port, mask | (1u << line));
}

void pic_clear_mask(uint8_t irq){
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t  line = (irq < 8) ? irq : irq - 8;
    uint8_t  mask = inb(port);
    outb(port, mask & ~(1u << line));
}

void pic_send_eoi(uint8_t irq){
    if(irq >= 8) outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}
