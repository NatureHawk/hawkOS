// src/mouse.c — PS/2 mouse driver (IRQ12), standard 3-byte packet protocol
#include <stdint.h>
#include "header/mouse.h"
#include "header/io.h"
#include "header/pic.h"
#include "header/gfx.h"
#include "header/vmmouse.h"
#include "header/kprintf.h"

#define PS2_DATA 0x60
#define PS2_CMD  0x64

static void wait_input_clear(void){ while (inb(PS2_CMD) & 0x02) { } }
static void wait_output_full(void){ while (!(inb(PS2_CMD) & 0x01)) { } }

static void mouse_write(uint8_t val){
    wait_input_clear();
    outb(PS2_CMD, 0xD4);   // next byte on 0x60 goes to the mouse, not the keyboard
    wait_input_clear();
    outb(PS2_DATA, val);
}
static uint8_t mouse_read(void){
    wait_output_full();
    return inb(PS2_DATA);
}

static uint8_t packet[4];
static int     packet_index = 0;
static int     packet_size = 3;
static int32_t cur_x = 0, cur_y = 0;
static int     left_btn = 0;
static volatile int wheel_accum = 0;
static int vm_active = 0;

int mouse_has_wheel(void){ return packet_size == 4 || vm_active; }
int mouse_is_absolute(void){ return vm_active; }

int mouse_wheel_take(void){
    int v = wheel_accum;
    wheel_accum = 0;
    return v;
}

static void mouse_set_rate(uint8_t rate){
    mouse_write(0xF3); mouse_read();
    mouse_write(rate); mouse_read();
}

void mouse_init(void){
    wait_input_clear(); outb(PS2_CMD, 0xA8);   // enable auxiliary (mouse) device

    wait_input_clear(); outb(PS2_CMD, 0x20);   // read controller configuration byte
    uint8_t status = mouse_read();
    status |= 0x02u;             // enable IRQ12
    status &= (uint8_t)~0x20u;   // enable mouse clock

    wait_input_clear(); outb(PS2_CMD, 0x60);
    wait_input_clear(); outb(PS2_DATA, status);

    mouse_write(0xF6); mouse_read();   // set defaults (ack)

    // The "magic knock": setting the sample rate to 200, then 100, then 80
    // makes an IntelliMouse-compatible device switch to reporting ID 3 and
    // send 4-byte packets with a scroll wheel in the fourth. A plain PS/2
    // mouse ignores the sequence and keeps reporting ID 0, so this is safe
    // to attempt unconditionally.
    mouse_set_rate(200);
    mouse_set_rate(100);
    mouse_set_rate(80);
    mouse_write(0xF2);                 // get device ID
    mouse_read();                      // ack
    uint8_t id = mouse_read();
    packet_size = (id == 3) ? 4 : 3;

    mouse_write(0xF4); mouse_read();   // enable data reporting (ack)

    cur_x = (int32_t)(gfx_available() ? gfx_width()  / 2 : 0);
    cur_y = (int32_t)(gfx_available() ? gfx_height() / 2 : 0);
    packet_index = 0;

    // Prefer absolute positioning when the host offers it. A relative mouse
    // cannot be made to track the host's own cursor: the two drift apart
    // permanently the first time ours clamps at a screen edge while the real
    // one keeps moving. The vmport reports where the pointer actually is.
    vm_active = (vmmouse_enable() == 0);

    kprintf("[mouse] %s, %s wheel\n",
            vm_active ? "absolute (vmmouse)" : "relative (PS/2)",
            (packet_size == 4 || vm_active) ? "with" : "no");
}

// Absolute coordinates arrive in a 0..65535 box regardless of the video
// mode, so they are scaled to the framebuffer here. 65535 * 1280 still fits
// in 32 bits, which keeps this off the 64-bit division helpers.
static void drain_vmmouse(void){
    uint32_t vx = 0, vy = 0;
    int btn = 0, wz = 0;
    int r;

    int32_t maxx = gfx_available() ? (int32_t)gfx_width()  - 1 : 0;
    int32_t maxy = gfx_available() ? (int32_t)gfx_height() - 1 : 0;

    while ((r = vmmouse_poll(&vx, &vy, &btn, &wz)) == 1){
        cur_x = (int32_t)(vx * (uint32_t)(maxx + 1) / 65536u);
        cur_y = (int32_t)(vy * (uint32_t)(maxy + 1) / 65536u);
        if (cur_x > maxx) cur_x = maxx;
        if (cur_y > maxy) cur_y = maxy;
        left_btn = (btn & VMMOUSE_LEFT) ? 1 : 0;
        wheel_accum += wz;
    }

    // The device faulted; drop back to the PS/2 stream rather than freezing
    // the pointer.
    if (r < 0) vm_active = 0;
}

void mouse_handler_c(void){
    uint8_t b = inb(PS2_DATA);

    if (vm_active){
        // The vmmouse still raises IRQ12 through the PS/2 aux channel, so the
        // byte has to be read to clear the controller even though the real
        // data comes from the vmport.
        (void)b;
        packet_index = 0;
        drain_vmmouse();
        pic_send_eoi(12);
        return;
    }

    if (packet_index == 0 && !(b & 0x08)) {
        pic_send_eoi(12);   // not a valid first byte — drop it and resync
        return;
    }
    packet[packet_index++] = b;

    if (packet_index >= packet_size){
        packet_index = 0;
        uint8_t flags = packet[0];
        int dx = packet[1];
        int dy = packet[2];
        if (flags & 0x10) dx -= 256;
        if (flags & 0x20) dy -= 256;
        left_btn = flags & 0x01;

        cur_x += dx;
        cur_y -= dy;   // PS/2 Y grows upward; screen Y grows downward

        int32_t maxx = gfx_available() ? (int32_t)gfx_width()  - 1 : 0;
        int32_t maxy = gfx_available() ? (int32_t)gfx_height() - 1 : 0;
        if (cur_x < 0) cur_x = 0; else if (cur_x > maxx) cur_x = maxx;
        if (cur_y < 0) cur_y = 0; else if (cur_y > maxy) cur_y = maxy;

        if (packet_size == 4){
            // Byte 3 carries the wheel as a 4-bit signed value; the upper
            // bits belong to the extra buttons and are not used here.
            int8_t z = (int8_t)(packet[3] & 0x0F);
            if (z & 0x08) z |= (int8_t)0xF0;
            wheel_accum += z;
        }
    }
    pic_send_eoi(12);
}

int32_t mouse_x(void){ return cur_x; }
int32_t mouse_y(void){ return cur_y; }
int     mouse_left_button(void){ return left_btn; }
