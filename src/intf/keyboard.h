#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include <stddef.h>

#define KEYBOARD_EVENT_TYPE_MAKE  0
#define KEYBOARD_EVENT_TYPE_BREAK 1

#define KEY_CODE_LEFT  0x4B
#define KEY_CODE_RIGHT 0x4D
#define KEY_CODE_UP    0x48
#define KEY_CODE_DOWN  0x50

struct KeyboardEvent {
    uint8_t type;
    uint16_t code;
    uint8_t ctrl;
    uint8_t shift;
};

typedef void (*keyboard_handler_t)(struct KeyboardEvent);

/** Initialize the keyboard IRQ handler and input ring buffer. */
void keyboard_init(void);
/** Install an optional keyboard event callback. */
void keyboard_set_handler(keyboard_handler_t handler);
/** Translate a PS/2 scan code to ASCII, or return zero if not printable. */
char to_ascii(uint16_t code);

/** @return Number of bytes currently buffered for readers. */
size_t keyboard_ring_available(void);
/** Remove up to count bytes without blocking; returns bytes read or -1. */
int keyboard_pop_bytes(char* buf, size_t count);
/** Block until input is available, then return up to count bytes or -1. */
int keyboard_read_blocking(char* buf, size_t count);

#endif