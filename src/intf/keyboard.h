#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

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

void keyboard_init(void);
void keyboard_set_handler(keyboard_handler_t handler);
char to_ascii(uint16_t code);

#endif