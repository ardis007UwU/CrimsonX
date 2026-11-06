#include "print.h"
#include "keyboard.h"
#include "shell.h"

void kernel_main(void) {
    print_clear();
    print_set_color(PRINT_COLOR_YELLOW, PRINT_COLOR_BLACK);
    print_str("Welcome to CrimsonX OS!\n\n");

    keyboard_init();
    keyboard_set_handler(shell_handle_input);
    
    shell_init();

    while (1);
}