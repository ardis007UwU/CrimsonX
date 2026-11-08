#ifndef STRING_H
#define STRING_H

#include <stddef.h>

/** Return the number of bytes before the terminating NUL. */
size_t strlen(const char* str);
/** Compare two NUL-terminated strings lexicographically. */
int strcmp(const char* s1, const char* s2);
/** Copy a NUL-terminated string; the destination must be large enough. */
char* strcpy(char* dest, const char* src);
/** Fill count bytes of dest with the low byte of byte. */
void* memset(void* dest, int byte, size_t count);
/** Copy count bytes from src to dest; the ranges must not overlap. */
void* memcpy(void* dest, const void* src, size_t count);
/** Compare count bytes lexicographically as unsigned bytes. */
int memcmp(const void* a, const void* b, size_t count);

#endif