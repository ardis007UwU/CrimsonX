#include "print.h"

static const size_t NUM_COLS = 80;
static const size_t NUM_ROWS = 25;

struct Char {
    uint8_t character;
    uint8_t color;
};

struct Char* buffer = (struct Char*) 0xb8000;
size_t col = 0;
size_t row = 0;
uint8_t color = PRINT_COLOR_WHITE | (PRINT_COLOR_BLACK << 4);

static void clear_row(size_t row_idx) {
    struct Char empty = (struct Char) {
        character: ' ',
        color: color
    };
    for (size_t i = 0; i < NUM_COLS; i++) {
        buffer[i + NUM_COLS * row_idx] = empty;
    }
}

void print_clear(void) {
    for (size_t i = 0; i < NUM_ROWS; i++) {
        clear_row(i);
    }
    col = 0;
    row = 0;
}

void print_char(char character) {
    if (character == '\n') {
        col = 0;
        row++;
    } else if (character == '\r') {
        col = 0;
    } else if (character == '\b') {
        if (col > 0) col--;
    } else {
        buffer[col + NUM_COLS * row] = (struct Char) {
            character: (uint8_t) character,
            color: color
        };
        col++;
    }

    if (col >= NUM_COLS) {
        col = 0;
        row++;
    }

    if (row >= NUM_ROWS) {
        for (size_t y = 1; y < NUM_ROWS; y++) {
            for (size_t x = 0; x < NUM_COLS; x++) {
                buffer[x + NUM_COLS * (y - 1)] = buffer[x + NUM_COLS * y];
            }
        }
        clear_row(NUM_ROWS - 1);
        row = NUM_ROWS - 1;
    }
}

void print_str(char* string) {
    for (size_t i = 0; string[i] != '\0'; i++) {
        print_char(string[i]);
    }
}

void print_set_color(uint8_t foreground, uint8_t background) {
    color = foreground + (background << 4);
}

void print_uint64_dec(uint64_t num) {
    if (num == 0) {
        print_char('0');
        return;
    }
    char buf[20];
    int i = 0;
    while (num > 0) {
        buf[i++] = (num % 10) + '0';
        num /= 10;
    }
    while (i > 0) {
        print_char(buf[--i]);
    }
}

void print_uint64_hex(uint64_t n) {
    char buf[16];
    int i = 0;
    const char* hex_chars = "0123456789ABCDEF";

    if (n == 0) {
        print_char('0');
        return;
    }

    while (n > 0) {
        buf[i++] = hex_chars[n % 16];
        n /= 16;
    }

    for (int j = i - 1; j >= 0; j--) {
        print_char(buf[j]);
    }
}

// --- Absolute Control Functions ---
size_t print_get_col(void) { return col; }
size_t print_get_row(void) { return row; }

void print_set_cursor(size_t c, size_t r) {
    col = c;
    row = r;
}

void print_put_char_at(char character, uint8_t fg_bg, size_t c, size_t r) {
    if (c >= NUM_COLS || r >= NUM_ROWS) return;
    buffer[c + NUM_COLS * r] = (struct Char) {
        character: (uint8_t) character,
        color: fg_bg
    };
}