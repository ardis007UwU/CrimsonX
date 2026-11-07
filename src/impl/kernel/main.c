#include "print.h"
#include "keyboard.h"
#include "shell.h"
#include "x86_64/port.h"
#include "x86_64/idt.h"
#include "x86_64/pic.h"
#include "x86_64/pit.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/kmalloc.h"

void kernel_main(void) {
    print_clear();
    print_set_color(PRINT_COLOR_YELLOW, PRINT_COLOR_BLACK);
    print_str("Welcome to CrimsonX OS!\n\n");

    idt_init();
    pic_init();

    // Unmask IRQ0 (Timer)
    uint8_t pic_mask = port_inb(0x21);
    port_outb(0x21, pic_mask & ~0x01);

    pit_init(100);
    idt_set_handler_timer(pit_handler);

    keyboard_init();
    keyboard_set_handler(shell_handle_input);

    asm volatile("sti");

    pmm_init(128ULL * 1024 * 1024);
    vmm_init();
    kmalloc_init();

    shell_init();

    while (1) {
        shell_update();
        asm volatile("hlt");
    }
}