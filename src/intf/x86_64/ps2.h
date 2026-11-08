#pragma once

#include <stdint.h>

/** Read one scan code from the PS/2 keyboard controller. */
uint8_t ps2_read_scan_code(void);
