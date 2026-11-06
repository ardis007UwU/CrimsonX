#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

#define KEYBOARD_EVENT_TYPE_MAKE  0
#define KEYBOARD_EVENT_TYPE_BREAK 1

struct KeyboardEvent {
    uint8_t type;
    uint16_t code;
};

typedef void (*keyboard_handler_t)(struct KeyboardEvent);

void keyboard_init(void);
void keyboard_set_handler(keyboard_handler_t handler);
char to_ascii(uint16_t code);

#endif