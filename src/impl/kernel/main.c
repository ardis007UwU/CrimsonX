#include "print.h"
#include "keyboard.h"
#include "shell.h"
#include "x86_64/port.h"
#include "x86_64/gdt.h"
#include "x86_64/idt.h"
#include "x86_64/pic.h"
#include "x86_64/pit.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/kmalloc.h"
#include "kernel/process.h"

void task_worker(void) {
    uint64_t ticks = 0;
    while (1) {
        const char* label = "[T1 Active: ";
        uint8_t color = PRINT_COLOR_GREEN | (PRINT_COLOR_BLACK << 4);

        for (size_t i = 0; label[i] != '\0'; i++) {
            print_put_char_at(label[i], color, 60 + i, 0);
        }

        char count_digit = '0' + (ticks % 10);
        print_put_char_at(count_digit, color, 72, 0);
        print_put_char_at(']', color, 73, 0);

        ticks++;
        process_sleep(1000);
    }
}

void kernel_main(void) {
    /* Replace the minimal boot GDT with the full one (kernel/user segments + TSS)
     * before the IDT is built. Interrupts are still off here. */
    gdt_init();

    print_clear();
    print_set_color(PRINT_COLOR_YELLOW, PRINT_COLOR_BLACK);
    print_str("Welcome to CrimsonX OS!\n\n");

    idt_init();
    pic_init();

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

    process_init();
    process_create(task_worker);

    shell_init();

    while (1) {
        shell_update();
        asm volatile("hlt");
    }
}