#ifndef PRINT_H
#define PRINT_H

#include <stdint.h>
#include <stddef.h>

#define PRINT_COLOR_BLACK      0
#define PRINT_COLOR_BLUE       1
#define PRINT_COLOR_GREEN      2
#define PRINT_COLOR_CYAN       3
#define PRINT_COLOR_RED        4
#define PRINT_COLOR_MAGENTA    5
#define PRINT_COLOR_BROWN      6
#define PRINT_COLOR_LIGHT_GRAY 7
#define PRINT_COLOR_DARK_GRAY  8
#define PRINT_COLOR_LIGHT_BLUE 9
#define PRINT_COLOR_LIGHT_GREEN 10
#define PRINT_COLOR_LIGHT_CYAN 11
#define PRINT_COLOR_LIGHT_RED  12
#define PRINT_COLOR_PINK       13
#define PRINT_COLOR_YELLOW     14
#define PRINT_COLOR_WHITE      15

/** Clear the display and reset the cursor to the origin. */
void print_clear(void);
/** Print one character at the current cursor position. */
void print_char(char character);
/** Print a NUL-terminated string at the current cursor position. */
void print_str(char* string);
/** Select the VGA foreground and background colors. */
void print_set_color(uint8_t foreground, uint8_t background);
/** Print an unsigned integer in decimal notation. */
void print_uint64_dec(uint64_t num);
/** Print an unsigned integer in hexadecimal notation. */
void print_uint64_hex(uint64_t n);

/** @return Current VGA cursor column. */
size_t print_get_col(void);
/** @return Current VGA cursor row. */
size_t print_get_row(void);
/** Set the VGA cursor position, clamped to the visible display. */
void print_set_cursor(size_t c, size_t r);
/** Draw a character at an absolute position using a packed VGA color. */
void print_put_char_at(char character, uint8_t color, size_t c, size_t r);

/** Initialize the COM1 serial output mirror. */
void print_serial_init(void);
/** Write a NUL-terminated string to the serial port when initialized. */
void serial_echo(const char* s);
/** Write a 64-bit value in hexadecimal to the serial port. */
void serial_put_hex64(uint64_t v);

#endif