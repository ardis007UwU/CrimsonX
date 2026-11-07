#include "shell.h"
#include "keyboard.h"
#include "print.h"
#include "string.h"
#include "x86_64/port.h"
#include "commands.h"

#define SHELL_BUF_SIZE 512

static char shell_buffer[SHELL_BUF_SIZE];
static char clip_buffer[SHELL_BUF_SIZE];
static int buf_len = 0;
static int cursor_pos = 0;
static int sel_anchor = -1;

static size_t prompt_row = 0;
static size_t prompt_len = 6;
static int max_rendered_len = 0;

static char pending_command[SHELL_BUF_SIZE];
static volatile int command_ready = 0;

static void copy_selection(void) {
    if (sel_anchor == -1 || sel_anchor == cursor_pos) return;
    int sel_min = (sel_anchor < cursor_pos) ? sel_anchor : cursor_pos;
    int sel_max = (sel_anchor < cursor_pos) ? cursor_pos : sel_anchor;
    int len = sel_max - sel_min;
    for (int i = 0; i < len; i++) {
        clip_buffer[i] = shell_buffer[sel_min + i];
    }
    clip_buffer[len] = '\0';
}

static void delete_selection(void) {
    if (sel_anchor == -1 || sel_anchor == cursor_pos) return;
    int sel_min = (sel_anchor < cursor_pos) ? sel_anchor : cursor_pos;
    int sel_max = (sel_anchor < cursor_pos) ? cursor_pos : sel_anchor;
    int len = sel_max - sel_min;
    for (int i = sel_min; i + len <= buf_len; i++) {
        shell_buffer[i] = shell_buffer[i + len];
    }
    buf_len -= len;
    cursor_pos = sel_min;
    sel_anchor = -1;
}

static void cut_selection(void) {
    if (sel_anchor == -1 || sel_anchor == cursor_pos) return;
    copy_selection();
    delete_selection();
}

static void paste_clipboard(void) {
    delete_selection();
    int clip_len = strlen(clip_buffer);
    if (clip_len == 0 || buf_len + clip_len >= SHELL_BUF_SIZE) return;
    for (int i = buf_len - 1; i >= cursor_pos; i--) {
        shell_buffer[i + clip_len] = shell_buffer[i];
    }
    for (int i = 0; i < clip_len; i++) {
        shell_buffer[cursor_pos + i] = clip_buffer[i];
    }
    buf_len += clip_len;
    cursor_pos += clip_len;
    shell_buffer[buf_len] = '\0';
}

static void shell_redraw(void) {
    size_t hw_cols = 80;
    size_t hw_rows = 25;

    /* Keep the prompt row synchronized with the actual terminal cursor whenever
     * command output has advanced the VGA hardware. */
    prompt_row = print_get_row();

    int sel_min = -1;
    int sel_max = -1;
    if (sel_anchor != -1 && sel_anchor != cursor_pos) {
        sel_min = (sel_anchor < cursor_pos) ? sel_anchor : cursor_pos;
        sel_max = (sel_anchor < cursor_pos) ? cursor_pos : sel_anchor;
    }

    int total_len = prompt_len + buf_len + 1;
    if (total_len > max_rendered_len) {
        max_rendered_len = total_len;
    }

    size_t required_rows = (max_rendered_len / hw_cols) + 1;
    while (prompt_row + required_rows > hw_rows) {
        print_set_cursor(0, hw_rows - 1);
        print_char('\n');
        if (prompt_row > 0) prompt_row--;
    }

    const char* prompt = "cxsh> ";
    uint8_t prompt_color = PRINT_COLOR_RED | (PRINT_COLOR_BLACK << 4);
    uint8_t arrow_color = PRINT_COLOR_WHITE | (PRINT_COLOR_BLACK << 4);

    for (size_t i = 0; i < 4; i++) print_put_char_at(prompt[i], prompt_color, i, prompt_row);
    for (size_t i = 4; i < 6; i++) print_put_char_at(prompt[i], arrow_color, i, prompt_row);

    size_t current_c = prompt_len;
    size_t current_r = prompt_row;

    for (int i = 0; i <= buf_len; i++) {
        uint8_t char_color;
        char c_to_print;

        if (i == buf_len) {
            c_to_print = ' ';
        } else {
            c_to_print = shell_buffer[i];
        }

        if (i == cursor_pos) {
            char_color = PRINT_COLOR_WHITE | (PRINT_COLOR_RED << 4);
        } else if (i >= sel_min && i < sel_max) {
            char_color = PRINT_COLOR_BLACK | (PRINT_COLOR_RED << 4);
        } else {
            char_color = PRINT_COLOR_WHITE | (PRINT_COLOR_BLACK << 4);
        }

        print_put_char_at(c_to_print, char_color, current_c, current_r);

        current_c++;
        if (current_c >= hw_cols) {
            current_c = 0;
            current_r++;
        }
    }

    /* Clear only from the end of the live prompt buffer to the end of the
     * current logical row/line; this avoids erasing previously rendered output
     * on rows that were already completed by command output. */
    uint8_t clear_color = PRINT_COLOR_WHITE | (PRINT_COLOR_BLACK << 4);
    size_t clear_c = (prompt_len + buf_len) % hw_cols;
    size_t clear_r = prompt_row + ((prompt_len + buf_len) / hw_cols);
    while (clear_r < hw_rows) {
        size_t limit = (clear_r == prompt_row) ? hw_cols - clear_c : hw_cols;
        for (size_t col = 0; col < limit; col++) {
            print_put_char_at(' ', clear_color, clear_c + col, clear_r);
        }
        clear_r++;
        clear_c = 0;
    }

    max_rendered_len = total_len;

    size_t final_c = (prompt_len + cursor_pos) % hw_cols;
    size_t final_r = prompt_row + ((prompt_len + cursor_pos) / hw_cols);
    print_set_cursor(final_c, final_r);
}

void shell_init(void) {
    buf_len = 0;
    cursor_pos = 0;
    sel_anchor = -1;
    max_rendered_len = 0;
    command_ready = 0;
    shell_buffer[0] = '\0';
    clip_buffer[0] = '\0';
    pending_command[0] = '\0';
    prompt_row = print_get_row();
    shell_redraw();
}

void shell_handle_input(struct KeyboardEvent event) {
    if (event.type != KEYBOARD_EVENT_TYPE_MAKE) return;

    if (command_ready) return;

    if (event.code == KEY_CODE_LEFT) {
        if (event.shift) {
            if (sel_anchor == -1) sel_anchor = cursor_pos;
            if (cursor_pos > 0) cursor_pos--;
        } else {
            sel_anchor = -1;
            if (cursor_pos > 0) cursor_pos--;
        }
        shell_redraw();
        return;
    }

    if (event.code == KEY_CODE_RIGHT) {
        if (event.shift) {
            if (sel_anchor == -1) sel_anchor = cursor_pos;
            if (cursor_pos < buf_len) cursor_pos++;
        } else {
            sel_anchor = -1;
            if (cursor_pos < buf_len) cursor_pos++;
        }
        shell_redraw();
        return;
    }

    if (event.ctrl) {
        if (event.code == 0x2E) { // Ctrl+C
            if (sel_anchor != -1 && sel_anchor != cursor_pos) {
                copy_selection();
                sel_anchor = -1;
                shell_redraw();
            } else if (event.shift) {
                copy_selection();
                sel_anchor = -1;
                shell_redraw();
            } else {
                print_char('\n');
                print_set_color(PRINT_COLOR_RED, PRINT_COLOR_BLACK);
                print_str("^C\n");
                buf_len = 0;
                cursor_pos = 0;
                sel_anchor = -1;
                max_rendered_len = 0;
                shell_buffer[0] = '\0';
                prompt_row = print_get_row();
                shell_redraw();
            }
            return;
        } else if (event.code == 0x2D) { // Ctrl+X
            cut_selection();
            shell_redraw();
            return;
        } else if (event.code == 0x2F) { // Ctrl+V
            paste_clipboard();
            shell_redraw();
            return;
        } else if (event.code == 0x26) { // Ctrl+L
            print_clear();
            prompt_row = 0;
            max_rendered_len = 0;
            shell_redraw();
            return;
        }
        return;
    }

    char c = to_ascii(event.code);
    if (c == 0) return;

    if (c == '\n') {
        print_char('\n');
        size_t total_rows = (prompt_len + buf_len) / 80;
        for (size_t r = 0; r < total_rows; r++) print_char('\n');

        strcpy(pending_command, shell_buffer);
        command_ready = 1;

        buf_len = 0;
        cursor_pos = 0;
        sel_anchor = -1;
        max_rendered_len = 0;
        shell_buffer[0] = '\0';
    } else if (c == '\b') {
        if (sel_anchor != -1 && sel_anchor != cursor_pos) {
            delete_selection();
        } else if (cursor_pos > 0) {
            for (int i = cursor_pos - 1; i < buf_len - 1; i++) {
                shell_buffer[i] = shell_buffer[i + 1];
            }
            buf_len--;
            cursor_pos--;
            shell_buffer[buf_len] = '\0';
        }
        shell_redraw();
    } else {
        delete_selection();
        if (buf_len < SHELL_BUF_SIZE - 1) {
            for (int i = buf_len; i > cursor_pos; i--) {
                shell_buffer[i] = shell_buffer[i - 1];
            }
            shell_buffer[cursor_pos] = c;
            buf_len++;
            cursor_pos++;
            shell_buffer[buf_len] = '\0';
        }
        shell_redraw();
    }
}

void shell_update(void) {
    if (command_ready) {
        execute_command(pending_command);
        command_ready = 0;
        /* Sync the shell prompt to the screen cursor after the command output has
         * advanced VGA rows, then clear the previous render bounds so ghost text
         * does not overwrite the active input line. */
        prompt_row = print_get_row();
        max_rendered_len = 0;
        shell_redraw();
    }
}