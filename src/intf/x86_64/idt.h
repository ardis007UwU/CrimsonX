#ifndef IDT_H
#define IDT_H

void idt_init(void);
void idt_set_handler_keyboard(void (*handler)(void));
void idt_set_handler_timer(void (*handler)(void));

#endif