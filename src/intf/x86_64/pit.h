#ifndef PIT_H
#define PIT_H

#include <stdint.h>

void pit_init(uint32_t frequency);
void pit_handler(void);
void pit_sleep(uint32_t ms);
uint64_t pit_get_ticks(void);
uint64_t pit_get_uptime_sec(void);

#endif