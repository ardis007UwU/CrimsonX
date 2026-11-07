#include "keyboard.h"
#include "x86_64/idt.h"
#include "x86_64/ps2.h"
#include "x86_64/pic.h"
#include "kernel/process.h"
#include "kernel/irq.h"
#include <stddef.h>
#include <stdint.h>

#define KEYBOARD_RING_SIZE 64   /* power of two */

static keyboard_handler_t current_handler = NULL;

/* ASCII ring buffer: filled by the IRQ handler, consumed by SYS_READ. */
#define KEYBOARD_RING_SIZE 64
static volatile char kb_ring[KEYBOARD_RING_SIZE];
static volatile size_t kb_head = 0;   /* consumer index */
static volatile size_t kb_tail = 0;   /* producer index */

/* Tasks parked inside keyboard_read_blocking: the IRQ handler wakes them
 * after pushing a char (wake-all; readers re-park if they still come up
 * short, so no single-waiter starvation). */
#define KB_WAIT_MAX 8
static task_t* kb_waiters[KB_WAIT_MAX];

static void kb_waiter_add(task_t* t) {
    for (int i = 0; i < KB_WAIT_MAX; i++) {
        if (kb_waiters[i] == t) return;
    }
    for (int i = 0; i < KB_WAIT_MAX; i++) {
        if (kb_waiters[i] == NULL) {
            kb_waiters[i] = t;
            return;
        }
    }
}

static void kb_waiter_del(task_t* t) {
    for (int i = 0; i < KB_WAIT_MAX; i++) {
        if (kb_waiters[i] == t) kb_waiters[i] = NULL;
    }
}

static void kb_wake_waiters(void) {
    for (int i = 0; i < KB_WAIT_MAX; i++) {
        task_t* t = kb_waiters[i];
        if (t && t->state == TASK_STATE_BLOCKED) process_wakeup(t);
    }
}

static uint8_t shift_l = 0;
static uint8_t shift_r = 0;
static uint8_t ctrl_l = 0;
static uint8_t caps_lock = 0;
static uint8_t is_e0 = 0;

static void kb_ring_push(char c) {
    size_t t = kb_tail;
    size_t next = (t + 1) & (KEYBOARD_RING_SIZE - 1);
    if (next == kb_head) return;   /* full: drop (no backpressure source) */
    kb_ring[t] = c;
    kb_tail = next;

    kb_wake_waiters();
}

/* IRQ context: translate make-codes into ASCII and feed the ring. */
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

    if (!is_break && !ctrl_l) {
        char c = to_ascii(code);
        if (c != 0) kb_ring_push(c);
    }

    is_e0 = 0;
    pic_eoi_master();
}

void keyboard_init(void) {
    idt_set_handler_keyboard(idt_handler_keyboard_user);
    current_handler = NULL;
    kb_head = 0;
    kb_tail = 0;
    for (int i = 0; i < KB_WAIT_MAX; i++) kb_waiters[i] = NULL;
    shift_l = 0;
    shift_r = 0;
    ctrl_l = 0;
    caps_lock = 0;
    is_e0 = 0;
}

void keyboard_set_handler(keyboard_handler_t handler) {
    current_handler = handler;
}

size_t keyboard_ring_available(void) {
    irq_flags_t f = irq_save();
    size_t n = (kb_tail - kb_head) & (KEYBOARD_RING_SIZE - 1);
    irq_restore(f);
    return n;
}

int keyboard_pop_bytes(char* buf, size_t count) {
    if (!buf) return -1;
    irq_flags_t f = irq_save();
    size_t i = 0;
    while (i < count && kb_head != kb_tail) {
        buf[i++] = kb_ring[kb_head];
        kb_head = (kb_head + 1) & (KEYBOARD_RING_SIZE - 1);
    }
    irq_restore(f);
    return (int)i;
}

int keyboard_read_blocking(char* buf, size_t count) {
    if (!buf || count == 0) return -1;

    task_t* self = process_current_task();

    for (;;) {
        irq_flags_t f = irq_save();
        size_t i = 0;
        while (i < count && kb_head != kb_tail) {
            buf[i++] = kb_ring[kb_head];
            kb_head = (kb_head + 1) & (KEYBOARD_RING_SIZE - 1);
        }
        if (i > 0) {
            irq_restore(f);
            return (int)i;
        }

        /* park: the IRQ handler wakes every parked reader once chars land in
         * the ring. We run with IF off (syscall context), so there is no
         * window between the empty check and the registration below. */
        kb_waiter_add(self);
        process_block();
        kb_waiter_del(self);
        irq_restore(f);
    }
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