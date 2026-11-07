#include "x86_64/pit.h"
#include "x86_64/port.h"
#include "x86_64/pic.h"

#define PIT_BASE_FREQ     1193182
#define PIT_COMMAND_PORT  0x43
#define PIT_CHANNEL0_PORT 0x40

static volatile uint64_t pit_ticks = 0;
static uint32_t pit_frequency = 100;

void pit_init(uint32_t frequency) {
    pit_frequency = frequency;
    uint16_t divisor = (uint16_t)(PIT_BASE_FREQ / frequency);

    /* Channel 0, Access mode low/high byte, Mode 3 (Square wave generator) */
    port_outb(PIT_COMMAND_PORT, 0x36);
    port_outb(PIT_CHANNEL0_PORT, (uint8_t)(divisor & 0xFF));
    port_outb(PIT_CHANNEL0_PORT, (uint8_t)((divisor >> 8) & 0xFF));
}

void pit_handler(void) {
    pit_ticks++;
    pic_eoi_master();
}

uint64_t pit_get_ticks(void) {
    return pit_ticks;
}

uint64_t pit_get_uptime_sec(void) {
    return pit_ticks / pit_frequency;
}

static uint8_t rtc_read_second(void) {
    /* Select register 0x00 (Seconds) */
    port_outb(0x70, 0x00);
    /* Read the value */
    return port_inb(0x71);
}

void pit_sleep(uint32_t ms) {
    /* Calculate target seconds to wait (minimum 1 second for noticeable delay) */
    uint32_t seconds_to_wait = ms / 1000;
    if (seconds_to_wait == 0 && ms > 0) {
        seconds_to_wait = 1;
    }

    uint8_t current_sec = rtc_read_second();
    uint8_t target_sec = current_sec;
    
    /* Calculate target second (RTC wraps at 60) */
    for (uint32_t i = 0; i < seconds_to_wait; i++) {
        target_sec++;
        if (target_sec >= 60) {
            target_sec = 0;
        }
    }

    /* Block until the RTC hardware actually ticks over to the target second */
    while (rtc_read_second() != target_sec) {
        asm volatile("pause");
    }
}