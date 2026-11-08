#ifndef DRIVERS_MOUSE_H
#define DRIVERS_MOUSE_H

#include <stdint.h>

#define MOUSE_BUTTON_LEFT   0x01
#define MOUSE_BUTTON_RIGHT  0x02
#define MOUSE_BUTTON_MIDDLE 0x04
#define MOUSE_SCREEN_WIDTH  1024
#define MOUSE_SCREEN_HEIGHT 768

typedef struct {
    int16_t delta_x;
    int16_t delta_y; /* Positive values move up on the screen. */
    uint8_t buttons;
    uint16_t x;
    uint16_t y;
} mouse_state_t;

typedef void (*mouse_handler_t)(const mouse_state_t* state);

/** Initialize the 8042 auxiliary port and enable the IRQ 12 mouse stream. */
int mouse_init(void);

/** Install an optional callback, invoked from IRQ context for each packet. */
void mouse_set_handler(mouse_handler_t handler);

/** Copy the most recently received mouse state. */
int mouse_get_state(mouse_state_t* state);

/** @return Current horizontal pointer coordinate, in [0, MOUSE_SCREEN_WIDTH). */
uint16_t mouse_get_x(void);

/** @return Current vertical pointer coordinate, in [0, MOUSE_SCREEN_HEIGHT). */
uint16_t mouse_get_y(void);

/** @return Current mouse button bitmask. */
uint8_t mouse_get_buttons(void);

#endif
