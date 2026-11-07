#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>

#define PMM_BLOCK_SIZE 4096
#define PMM_BLOCKS_PER_BYTE 8

void pmm_init(uint64_t mem_size);
void* pmm_alloc_block(void);
void pmm_free_block(void* ptr);

uint64_t pmm_get_free_block_count(void);
uint64_t pmm_get_used_block_count(void);

#endif