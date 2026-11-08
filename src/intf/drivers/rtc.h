#ifndef DRIVERS_RTC_H
#define DRIVERS_RTC_H

#include <stdint.h>

typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} rtc_datetime_t;

/** Read a consistent calendar snapshot from the CMOS real-time clock. */
int rtc_get_datetime(rtc_datetime_t* datetime);

/** Return the CMOS calendar as seconds since the Unix epoch (UTC). */
int rtc_get_unix_time(uint64_t* seconds);

#endif
