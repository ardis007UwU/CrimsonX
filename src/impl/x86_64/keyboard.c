#include "keyboard.h"
#include "x86_64/idt.h"
#include "x86_64/ps2.h"
#include "x86_64/pic.h"
#include <stddef.h>
#include <stdint.h>

static keyboard_handler_t current_handler = NULL;

static uint8_t shift_l = 0;
static uint8_t shift_r = 0;
static uint8_t ctrl_l = 0;
static uint8_t caps_lock = 0;
static uint8_t is_e0 = 0;

static void idt_handler_keyboard_user(void) {
    uint8_t scancode = ps2_read_scan_code();

    if (scancode == 0xE0) {
        is_e0 = 1;
        pic_eoi_master();
        return;
    }

    uint8_t is_break = (scancode & 0x80) != 0;
    uint8_t code = scancode & ~0x80;

    if (code == 0x2A) {
        shift_l = !is_break;
    } else if (code == 0x36) {
        shift_r = !is_break;
    } else if (code == 0x1D) {
        ctrl_l = !is_break;
    } else if (code == 0x3A && !is_break) {
        caps_lock = !caps_lock;
    }

    if (current_handler != NULL) {
        struct KeyboardEvent event;
        event.type = is_break ? KEYBOARD_EVENT_TYPE_BREAK : KEYBOARD_EVENT_TYPE_MAKE;
        event.code = code;
        event.ctrl = ctrl_l;
        event.shift = (shift_l || shift_r);
        current_handler(event);
    }

    is_e0 = 0;
    pic_eoi_master();
}

void keyboard_init(void) {
    idt_set_handler_keyboard(idt_handler_keyboard_user);
    current_handler = NULL;
    shift_l = 0;
    shift_r = 0;
    ctrl_l = 0;
    caps_lock = 0;
    is_e0 = 0;
}

void keyboard_set_handler(keyboard_handler_t handler) {
    current_handler = handler;
}

char to_ascii(uint16_t code) {
    if (code > 0x39) return 0;

    uint8_t shift_active = (shift_l || shift_r);

    static const char ascii_nomod[58] = {
        0,    0,   '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
        '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
        0,    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,
        '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,   '*', 0,   ' '
    };

    static const char ascii_shift[58] = {
        0,    0,   '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
        '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
        0,    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,
        '|',  'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,   '*', 0,   ' '
    };

    char c_nomod = ascii_nomod[code];
    char c_shift = ascii_shift[code];

    if (c_nomod >= 'a' && c_nomod <= 'z') {
        if (shift_active ^ caps_lock) {
            return c_shift;
        } else {
            return c_nomod;
        }
    } else {
        if (shift_active) {
            return c_shift;
        } else {
            return c_nomod;
        }
    }
}