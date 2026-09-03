#pragma once
#include <stdint.h>

typedef struct {
    uint8_t  sec, min, hour, day, month;
    uint16_t year;
} rtc_time_t;

// Reads the current wall-clock time from the CMOS RTC (ports 0x70/0x71).
void rtc_read(rtc_time_t* out);
