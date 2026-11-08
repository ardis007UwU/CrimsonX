#ifndef IDT_H
#define IDT_H

/** Install the interrupt descriptor table and exception/IRQ stubs. */
void idt_init(void);
/** Register the keyboard interrupt callback. */
void idt_set_handler_keyboard(void (*handler)(void));
/** Register the timer interrupt callback. */
void idt_set_handler_timer(void (*handler)(void));

#endif