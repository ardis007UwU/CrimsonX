#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>

#define PMM_BLOCK_SIZE 4096
#define PMM_BLOCKS_PER_BYTE 8

/** Initialize the physical allocator for the supplied memory size in bytes. */
void pmm_init(uint64_t mem_size);
/** Allocate one physical page; returns NULL when memory is exhausted. */
void* pmm_alloc_block(void);
/** Return one allocated physical page to the allocator. */
void pmm_free_block(void* ptr);

/** Allocate count physically contiguous pages, or return NULL if none fit. */
void* pmm_alloc_contiguous(size_t count);

/** @return Number of currently free physical pages. */
uint64_t pmm_get_free_block_count(void);
/** @return Number of currently allocated physical pages. */
uint64_t pmm_get_used_block_count(void);

#endif