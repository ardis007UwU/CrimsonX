#ifndef PIT_H
#define PIT_H

#include <stdint.h>

/** Program the PIT to generate ticks at the requested frequency. */
void pit_init(uint32_t frequency);
/** Advance the global tick counter from the timer interrupt. */
void pit_handler(void);
/** Sleep for at least the requested number of milliseconds. */
void pit_sleep(uint32_t ms);
/** @return Monotonic PIT ticks since initialization. */
uint64_t pit_get_ticks(void);
/** @return Uptime in whole seconds. */
uint64_t pit_get_uptime_sec(void);

#endif