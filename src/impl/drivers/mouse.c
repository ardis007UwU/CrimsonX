#include "drivers/mouse.h"
#include "kernel/irq.h"
#include "x86_64/idt.h"
#include "x86_64/pic.h"
#include "x86_64/port.h"
#include <stddef.h>

#define PS2_DATA_PORT       0x60
#define PS2_STATUS_PORT     0x64
#define PS2_COMMAND_PORT    0x64

#define PS2_STATUS_OUTPUT_FULL 0x01
#define PS2_STATUS_INPUT_FULL  0x02
#define PS2_STATUS_AUX_DATA    0x20

#define PS2_COMMAND_READ_BYTE  0x20
#define PS2_COMMAND_WRITE_BYTE 0x60
#define PS2_COMMAND_ENABLE_AUX 0xA8
#define PS2_COMMAND_WRITE_AUX  0xD4

#define MOUSE_COMMAND_DEFAULTS 0xF6
#define MOUSE_COMMAND_ENABLE   0xF4
#define MOUSE_RESPONSE_ACK     0xFA
#define PS2_WAIT_LIMIT         100000U

static volatile uint8_t packet[3];
static volatile uint8_t packet_index;
static volatile mouse_state_t current_state;
static mouse_handler_t current_handler;

static int ps2_wait_input_empty(void) {
    for (uint32_t i = 0; i < PS2_WAIT_LIMIT; i++) {
        if (!(port_inb(PS2_STATUS_PORT) & PS2_STATUS_INPUT_FULL)) return 0;
    }
    return -1;
}

static int ps2_wait_output_full(int auxiliary) {
    for (uint32_t i = 0; i < PS2_WAIT_LIMIT; i++) {
        uint8_t status = port_inb(PS2_STATUS_PORT);
        if (!(status & PS2_STATUS_OUTPUT_FULL)) continue;
        if (!!(status & PS2_STATUS_AUX_DATA) == !!auxiliary) return 0;
    }
    return -1;
}

static int ps2_controller_command(uint8_t command) {
    if (ps2_wait_input_empty() != 0) return -1;
    port_outb(PS2_COMMAND_PORT, command);
    return 0;
}

static int ps2_send_mouse_byte(uint8_t value) {
    if (ps2_controller_command(PS2_COMMAND_WRITE_AUX) != 0 ||
        ps2_wait_input_empty() != 0) return -1;
    port_outb(PS2_DATA_PORT, value);
    if (ps2_wait_output_full(1) != 0) return -1;
    return port_inb(PS2_DATA_PORT) == MOUSE_RESPONSE_ACK ? 0 : -1;
}

static void mouse_irq_handler(void) {
    uint8_t status = port_inb(PS2_STATUS_PORT);
    if (!(status & PS2_STATUS_OUTPUT_FULL) ||
        !(status & PS2_STATUS_AUX_DATA)) return;

    uint8_t value = port_inb(PS2_DATA_PORT);
    if (packet_index == 0) {
        if (!(value & 0x08U)) return;
        packet[packet_index++] = value;
        return;
    }

    packet[packet_index++] = value;
    if (packet_index < 3) return;
    packet_index = 0;

    uint8_t flags = packet[0];
    if (flags & 0xC0U) return;

    int16_t delta_x = packet[1];
    int16_t delta_y = packet[2];
    if (packet[1] & 0x80U) delta_x -= 256;
    if (packet[2] & 0x80U) delta_y -= 256;
    current_state.delta_x = delta_x;
    current_state.delta_y = delta_y;
    current_state.buttons = flags & 0x07U;
    if (current_handler) {
        mouse_state_t event = {
            .delta_x = current_state.delta_x,
            .delta_y = current_state.delta_y,
            .buttons = current_state.buttons
        };
        current_handler(&event);
    }
}

int mouse_init(void) {
    irq_flags_t flags = irq_save();
    packet_index = 0;
    current_state.delta_x = 0;
    current_state.delta_y = 0;
    current_state.buttons = 0;
    current_handler = NULL;
    idt_set_handler_mouse(mouse_irq_handler);

    int result = -1;
    if (ps2_controller_command(PS2_COMMAND_ENABLE_AUX) != 0) goto done;

    if (ps2_controller_command(PS2_COMMAND_READ_BYTE) != 0 ||
        ps2_wait_output_full(0) != 0) goto done;
    uint8_t command_byte = port_inb(PS2_DATA_PORT);
    command_byte |= 0x02U;
    command_byte &= (uint8_t)~0x20U;

    if (ps2_controller_command(PS2_COMMAND_WRITE_BYTE) != 0 ||
        ps2_wait_input_empty() != 0) goto done;
    port_outb(PS2_DATA_PORT, command_byte);

    if (ps2_send_mouse_byte(MOUSE_COMMAND_DEFAULTS) != 0 ||
        ps2_send_mouse_byte(MOUSE_COMMAND_ENABLE) != 0) goto done;

    result = 0;

done:
    irq_restore(flags);
    if (result == 0) {
        pic_set_irq_enabled(12, 1);
    } else {
        pic_set_irq_enabled(12, 0);
    }
    return result;
}

void mouse_set_handler(mouse_handler_t handler) {
    irq_flags_t flags = irq_save();
    current_handler = handler;
    irq_restore(flags);
}

int mouse_get_state(mouse_state_t* state) {
    if (!state) return -1;
    irq_flags_t flags = irq_save();
    state->delta_x = current_state.delta_x;
    state->delta_y = current_state.delta_y;
    state->buttons = current_state.buttons;
    irq_restore(flags);
    return 0;
}
