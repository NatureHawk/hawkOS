// src/rtc.c — CMOS real-time clock driver
#include "header/rtc.h"
#include "header/io.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static uint8_t cmos_read(uint8_t reg){
    outb(CMOS_ADDR, reg);
    io_wait();
    return inb(CMOS_DATA);
}

static int rtc_update_in_progress(void){
    outb(CMOS_ADDR, 0x0A);
    return inb(CMOS_DATA) & 0x80;
}

static uint8_t bcd_to_bin(uint8_t v){ return (uint8_t)((v & 0x0F) + (v >> 4) * 10); }

void rtc_read(rtc_time_t* out){
    while (rtc_update_in_progress()) { }

    uint8_t sec  = cmos_read(0x00);
    uint8_t min  = cmos_read(0x02);
    uint8_t hour = cmos_read(0x04);
    uint8_t day  = cmos_read(0x07);
    uint8_t mon  = cmos_read(0x08);
    uint8_t yr   = cmos_read(0x09);
    uint8_t regB = cmos_read(0x0B);

    if (!(regB & 0x04)) {   // values are packed BCD, not binary
        sec  = bcd_to_bin(sec);
        min  = bcd_to_bin(min);
        hour = bcd_to_bin((uint8_t)(hour & 0x7Fu)) | (uint8_t)(hour & 0x80u);
        day  = bcd_to_bin(day);
        mon  = bcd_to_bin(mon);
        yr   = bcd_to_bin(yr);
    }
    if (!(regB & 0x02) && (hour & 0x80u)) {   // 12-hour mode, PM flag set
        hour = (uint8_t)(((hour & 0x7Fu) + 12u) % 24u);
    }

    out->sec = sec; out->min = min; out->hour = hour;
    out->day = day; out->month = mon;
    out->year = (uint16_t)(2000u + yr);
}
