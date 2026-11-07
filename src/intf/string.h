#ifndef STRING_H
#define STRING_H

#include <stddef.h>

size_t strlen(const char* str);
int strcmp(const char* s1, const char* s2);
char* strcpy(char* dest, const char* src);
void* memset(void* dest, int byte, size_t count);
void* memcpy(void* dest, const void* src, size_t count);
int memcmp(const void* a, const void* b, size_t count);

#endif