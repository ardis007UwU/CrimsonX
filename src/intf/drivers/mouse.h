#ifndef DRIVERS_MOUSE_H
#define DRIVERS_MOUSE_H

#include <stdint.h>

#define MOUSE_BUTTON_LEFT   0x01
#define MOUSE_BUTTON_RIGHT  0x02
#define MOUSE_BUTTON_MIDDLE 0x04

typedef struct {
    int16_t delta_x;
    int16_t delta_y; /* Positive values move up on the screen. */
    uint8_t buttons;
} mouse_state_t;

typedef void (*mouse_handler_t)(const mouse_state_t* state);

/** Initialize the 8042 auxiliary port and enable the IRQ 12 mouse stream. */
int mouse_init(void);

/** Install an optional callback, invoked from IRQ context for each packet. */
void mouse_set_handler(mouse_handler_t handler);

/** Copy the most recently received mouse state. */
int mouse_get_state(mouse_state_t* state);

#endif
