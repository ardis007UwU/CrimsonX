#pragma once

#include <stdint.h>

/** Read one byte from an x86 I/O port. */
uint8_t port_inb(uint16_t port);
/** Read one word from an x86 I/O port. */
uint16_t port_inw(uint16_t port);
/** Write one byte to an x86 I/O port. */
void port_outb(uint16_t port, uint8_t data);
/** Write one word to an x86 I/O port. */
void port_outw(uint16_t port, uint16_t data);
/** Delay briefly using the legacy port-0x80 mechanism. */
void port_wait(void);
