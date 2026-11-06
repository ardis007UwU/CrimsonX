#include "shell.h"
#include "print.h"
#include "string.h"

#define SHELL_BUF_SIZE 256

static char shell_buffer[SHELL_BUF_SIZE];
static int buf_pos = 0;

static void print_prompt(void) {
    print_set_color(PRINT_COLOR_GREEN, PRINT_COLOR_BLACK);
    print_str("cxsh");
    print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK);
    print_str("> ");
}

static void execute_command(char* cmd) {
    if (cmd[0] == '\0') return;

    if (strcmp(cmd, "HELP") == 0 || strcmp(cmd, "help") == 0) {
        print_set_color(PRINT_COLOR_CYAN, PRINT_COLOR_BLACK);
        print_str("cxsh commands:\n");
        print_str("  help  - Show commands\n");
        print_str("  clear - Clear screen\n");
    } else if (strcmp(cmd, "CLEAR") == 0 || strcmp(cmd, "clear") == 0) {
        print_clear();
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