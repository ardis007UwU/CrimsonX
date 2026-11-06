#include "shell.h"
#include "print.h"
#include "string.h"
#include "x86_64/port.h"

#define SHELL_BUF_SIZE 256

static char shell_buffer[SHELL_BUF_SIZE];
static int buf_pos = 0;

static void print_prompt(void) {
    print_set_color(PRINT_COLOR_GREEN, PRINT_COLOR_BLACK);
    print_str("cxsh");
    print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK);
    print_str("> ");
}

static void sys_reboot(void) {
    uint8_t status = 0x02;
    while (status & 0x02) {
        status = port_inb(0x64);
    }
    port_outb(0x64, 0xFE);
    asm volatile("cli; hlt");
}

static void execute_command(char* cmd) {
    if (cmd[0] == '\0') return;

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "HELP") == 0) {
        print_set_color(PRINT_COLOR_CYAN, PRINT_COLOR_BLACK);
        print_str("cxsh commands:\n");
        print_str("  help    - Show available commands\n");
        print_str("  clear   - Clear terminal screen\n");
        print_str("  reboot  - Restart the CPU via PS/2 controller\n");
        print_str("  echo    - Print text back to screen\n");
    } else if (strcmp(cmd, "clear") == 0 || strcmp(cmd, "CLEAR") == 0) {
        print_clear();
    } else if (strcmp(cmd, "reboot") == 0 || strcmp(cmd, "REBOOT") == 0) {
        print_set_color(PRINT_COLOR_YELLOW, PRINT_COLOR_BLACK);
        print_str("Rebooting CrimsonX...\n");
        sys_reboot();
    } else if (cmd[0] == 'e' && cmd[1] == 'c' && cmd[2] == 'h' && cmd[3] == 'o' && cmd[4] == ' ') {
        print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK);
        print_str(cmd + 5);
        print_str("\n");
    } else {
        print_set_color(PRINT_COLOR_RED, PRINT_COLOR_BLACK);
        print_str("cxsh: command not found: ");
        print_str(cmd);
        print_str("\n");
    }
}

void shell_init(void) {
    buf_pos = 0;
    shell_buffer[0] = '\0';
    print_prompt();
}

void shell_handle_input(struct KeyboardEvent event) {
    if (event.type != KEYBOARD_EVENT_TYPE_MAKE) return;

    char c = to_ascii(event.code);
    if (c == 0) return;

    if (c == '\n') {
        print_char('\n');
        execute_command(shell_buffer);
        buf_pos = 0;
        shell_buffer[0] = '\0';
        print_prompt();
    } else if (c == '\b') {
        if (buf_pos > 0) {
            buf_pos--;
            shell_buffer[buf_pos] = '\0';
            print_char('\b');
        }
    } else {
        if (buf_pos < SHELL_BUF_SIZE - 1) {
            shell_buffer[buf_pos++] = c;
            shell_buffer[buf_pos] = '\0';
            print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK);
            print_char(c);
        }
    }
}