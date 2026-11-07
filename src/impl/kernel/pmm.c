#include "mm/pmm.h"
#include "print.h"

#define MAX_MEM_MB 512
#define PMM_MAX_BLOCKS ((MAX_MEM_MB * 1024 * 1024) / PMM_BLOCK_SIZE)

static uint8_t pmm_bitmap[PMM_MAX_BLOCKS / 8];
static uint64_t pmm_total_blocks = 0;
static uint64_t pmm_used_blocks = 0;

static inline void pmm_bitmap_set(uint64_t bit) {
    pmm_bitmap[bit / 8] |= (1 << (bit % 8));
}

static inline void pmm_bitmap_unset(uint64_t bit) {
    pmm_bitmap[bit / 8] &= ~(1 << (bit % 8));
}

static inline uint8_t pmm_bitmap_test(uint64_t bit) {
    return (pmm_bitmap[bit / 8] & (1 << (bit % 8))) != 0;
}

void pmm_init(uint64_t mem_size) {
    pmm_total_blocks = mem_size / PMM_BLOCK_SIZE;
    if (pmm_total_blocks > PMM_MAX_BLOCKS) {
        pmm_total_blocks = PMM_MAX_BLOCKS;
    }

    pmm_used_blocks = pmm_total_blocks;

    for (uint64_t i = 0; i < pmm_total_blocks / 8; i++) {
        pmm_bitmap[i] = 0xFF;
    }

    uint64_t start_block = 0x200000 / PMM_BLOCK_SIZE;
    for (uint64_t i = start_block; i < pmm_total_blocks; i++) {
        pmm_bitmap_unset(i);
        pmm_used_blocks--;
    }
}

void* pmm_alloc_block(void) {
    if (pmm_used_blocks >= pmm_total_blocks) return NULL;

    for (uint64_t i = 0; i < pmm_total_blocks; i++) {
        if (!pmm_bitmap_test(i)) {
            pmm_bitmap_set(i);
            pmm_used_blocks++;
            return (void*)(i * PMM_BLOCK_SIZE);
        }
    }
    return NULL;
}

void pmm_free_block(void* ptr) {
    uint64_t frame = ((uint64_t)ptr) / PMM_BLOCK_SIZE;
    if (pmm_bitmap_test(frame)) {
        pmm_bitmap_unset(frame);
        pmm_used_blocks--;
    }
}

uint64_t pmm_get_free_block_count(void) {
    return pmm_total_blocks - pmm_used_blocks;
}

uint64_t pmm_get_used_block_count(void) {
    return pmm_used_blocks;
}