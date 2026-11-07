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

void pit_sleep(uint32_t ms) {
    uint64_t ticks_to_wait = ((uint64_t)ms * (uint64_t)pit_frequency) / 1000;
    if (ticks_to_wait == 0 && ms > 0) {
        ticks_to_wait = 1;
    }

    uint64_t target_ticks = pit_ticks + ticks_to_wait;
    
    while (pit_ticks < target_ticks) {
        // sti: Guarantees hardware interrupts are enabled so the timer can fire.
        // hlt: Sleeps the CPU until the next interrupt to save power.
        // "memory": Tells the GCC compiler it MUST reload variables from RAM.
        asm volatile("sti; hlt" : : : "memory");
    }
}