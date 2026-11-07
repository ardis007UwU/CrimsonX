#include "print.h"
#include "keyboard.h"
#include "shell.h"
#include "x86_64/idt.h"
#include "x86_64/pic.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/kmalloc.h"

void kernel_main(void) {
    print_clear();
    print_set_color(PRINT_COLOR_YELLOW, PRINT_COLOR_BLACK);
    print_str("Welcome to CrimsonX OS!\n\n");

    idt_init();
    pic_init();

    keyboard_init();
    keyboard_set_handler(shell_handle_input);

    asm volatile("sti");

    // Initialize Memory Subsystems in order:
    // 1. Physical Memory Manager
    pmm_init(128ULL * 1024 * 1024);
    
    // 2. Virtual Memory Manager (Paging)
    vmm_init();
    
    // 3. Kernel Heap
    kmalloc_init();

    shell_init();

    while (1);
}