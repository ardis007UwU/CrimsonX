#include "drivers/rtc.h"
#include "kernel/irq.h"
#include "x86_64/port.h"

#define RTC_COMMAND_PORT 0x70
#define RTC_DATA_PORT    0x71

#define RTC_SECONDS      0x00
#define RTC_MINUTES      0x02
#define RTC_HOURS        0x04
#define RTC_DAY          0x07
#define RTC_MONTH        0x08
#define RTC_YEAR         0x09
#define RTC_STATUS_A     0x0A
#define RTC_STATUS_B     0x0B
#define RTC_CENTURY      0x32

#define RTC_UPDATE_IN_PROGRESS 0x80
#define RTC_24_HOUR_MODE       0x02
#define RTC_BINARY_MODE        0x04
#define RTC_PM                 0x80

typedef struct {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint8_t year;
    uint8_t century;
    uint8_t status_b;
} rtc_raw_t;

static uint8_t rtc_read_register(uint8_t reg) {
    port_outb(RTC_COMMAND_PORT, reg);
    return port_inb(RTC_DATA_PORT);
}

static int rtc_wait_update_complete(void) {
    for (uint32_t attempt = 0; attempt < 1000000U; attempt++) {
        if (!(rtc_read_register(RTC_STATUS_A) & RTC_UPDATE_IN_PROGRESS)) return 0;
    }
    return -1;
}

static void rtc_read_raw(rtc_raw_t* raw) {
    raw->second = rtc_read_register(RTC_SECONDS);
    raw->minute = rtc_read_register(RTC_MINUTES);
    raw->hour = rtc_read_register(RTC_HOURS);
    raw->day = rtc_read_register(RTC_DAY);
    raw->month = rtc_read_register(RTC_MONTH);
    raw->year = rtc_read_register(RTC_YEAR);
    raw->century = rtc_read_register(RTC_CENTURY);
    raw->status_b = rtc_read_register(RTC_STATUS_B);
}

static int rtc_raw_equal(const rtc_raw_t* a, const rtc_raw_t* b) {
    return a->second == b->second && a->minute == b->minute &&
           a->hour == b->hour && a->day == b->day &&
           a->month == b->month && a->year == b->year &&
           a->century == b->century && a->status_b == b->status_b;
}

static uint8_t rtc_bcd_to_binary(uint8_t value) {
    return (uint8_t)((value & 0x0FU) + ((value >> 4) * 10U));
}

static int rtc_is_leap_year(uint16_t year) {
    return (year % 4U == 0U) &&
           ((year % 100U != 0U) || (year % 400U == 0U));
}

static int rtc_decode(const rtc_raw_t* raw, rtc_datetime_t* datetime) {
    uint8_t hour = raw->hour;
    uint8_t pm = hour & RTC_PM;
    hour &= (uint8_t)~RTC_PM;

    uint8_t second = raw->second;
    uint8_t minute = raw->minute;
    uint8_t day = raw->day;
    uint8_t month = raw->month;
    uint8_t year = raw->year;
    uint8_t century = raw->century;
    int binary = (raw->status_b & RTC_BINARY_MODE) != 0;
    int mode_24_hour = (raw->status_b & RTC_24_HOUR_MODE) != 0;

    if (!binary) {
        second = rtc_bcd_to_binary(second);
        minute = rtc_bcd_to_binary(minute);
        hour = rtc_bcd_to_binary(hour);
        day = rtc_bcd_to_binary(day);
        month = rtc_bcd_to_binary(month);
        year = rtc_bcd_to_binary(year);
        if (century != 0 && century != 0xFFU) {
            century = rtc_bcd_to_binary(century);
        }
    }

    if (!mode_24_hour) {
        if (hour < 1 || hour > 12) return -1;
        if (hour == 12) hour = 0;
        if (pm) hour = (uint8_t)(hour + 12);
    } else if (pm) {
        return -1;
    }

    uint16_t full_year;
    if (century != 0 && century != 0xFFU && century <= 99) {
        full_year = (uint16_t)(century * 100U + year);
    } else {
        full_year = (year >= 70) ? (uint16_t)(1900U + year)
                                 : (uint16_t)(2000U + year);
    }

    if (full_year < 1970 || month < 1 || month > 12 ||
        hour > 23 || minute > 59 || second > 59) {
        return -1;
    }
    static const uint8_t month_days[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    uint8_t max_day = month_days[month - 1];
    if (month == 2 && rtc_is_leap_year(full_year)) max_day++;
    if (day < 1 || day > max_day) return -1;

    datetime->year = full_year;
    datetime->month = month;
    datetime->day = day;
    datetime->hour = hour;
    datetime->minute = minute;
    datetime->second = second;
    return 0;
}

int rtc_get_datetime(rtc_datetime_t* datetime) {
    if (!datetime) return -1;

    irq_flags_t flags = irq_save();
    rtc_raw_t previous;
    rtc_raw_t current;
    int result = -1;

    for (uint32_t attempt = 0; attempt < 16; attempt++) {
        if (rtc_wait_update_complete() != 0) break;
        rtc_read_raw(&previous);
        if (rtc_wait_update_complete() != 0) break;
        rtc_read_raw(&current);
        if (!rtc_raw_equal(&previous, &current)) continue;
        if (rtc_decode(&current, datetime) == 0) result = 0;
        break;
    }

    irq_restore(flags);
    return result;
}

int rtc_get_unix_time(uint64_t* seconds) {
    if (!seconds) return -1;
    rtc_datetime_t datetime;
    if (rtc_get_datetime(&datetime) != 0) return -1;

    uint64_t days = 0;
    for (uint16_t year = 1970; year < datetime.year; year++) {
        days += rtc_is_leap_year(year) ? 366U : 365U;
    }

    static const uint8_t month_days[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    for (uint8_t month = 1; month < datetime.month; month++) {
        days += month_days[month - 1];
        if (month == 2 && rtc_is_leap_year(datetime.year)) days++;
    }
    days += (uint64_t)datetime.day - 1U;

    *seconds = days * 86400U + (uint64_t)datetime.hour * 3600U +
               (uint64_t)datetime.minute * 60U + datetime.second;
    return 0;
}
