#ifndef KMALLOC_H
#define KMALLOC_H

#include <stddef.h>
#include <stdint.h>

/** Initialize the kernel heap allocator. */
void kmalloc_init(void);
/** Allocate size bytes from the kernel heap, or return NULL on failure. */
void* kmalloc(size_t size);
/** Release a pointer returned by kmalloc; NULL is accepted. */
void kfree(void* ptr);

#endif