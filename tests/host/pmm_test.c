/* Host-side tests for src/impl/kernel/pmm.c, mainly pmm_alloc_contiguous. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "mm/pmm.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { failures++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define MEM (16ULL * 1024 * 1024)
#define USABLE_BLOCKS ((MEM - 0x200000) / PMM_BLOCK_SIZE)   /* PMM starts at 2 MB */

int main(void) {
    pmm_init(MEM);
    CHECK(pmm_get_free_block_count() == USABLE_BLOCKS);

    /* A 1024x768x32-bit backbuffer needs 768 contiguous 4 KiB pages. */
    uint64_t backbuffer = (uint64_t)pmm_alloc_contiguous(768);
    CHECK(backbuffer >= 0x200000 && backbuffer % PMM_BLOCK_SIZE == 0);
    CHECK(pmm_get_free_block_count() == USABLE_BLOCKS - 768);
    if (backbuffer != 0) {
        for (uint64_t i = 0; i < 768; i++) {
            pmm_free_block((void*)(backbuffer + i * PMM_BLOCK_SIZE));
        }
    }
    CHECK(pmm_get_free_block_count() == USABLE_BLOCKS);

    /* Basic contiguous allocation */
    uint64_t a = (uint64_t)pmm_alloc_contiguous(5);
    CHECK(a >= 0x200000 && a % PMM_BLOCK_SIZE == 0);
    CHECK(pmm_get_free_block_count() == USABLE_BLOCKS - 5);
    CHECK(pmm_alloc_contiguous(0) == NULL);
    CHECK((uint64_t)pmm_alloc_contiguous(1) == a + 5 * PMM_BLOCK_SIZE);   /* single-block path */

    /* Consume everything, then punch holes: free blocks exist, but no run of 2 */
    uint64_t held[USABLE_BLOCKS];
    int n = 0;
    void* p;
    while ((p = pmm_alloc_block()) != NULL) held[n++] = (uint64_t)p;
    CHECK(pmm_get_free_block_count() == 0);
    CHECK(pmm_alloc_contiguous(1) == NULL);

    for (int i = 0; i < n; i += 2) pmm_free_block((void*)held[i]);    /* every other block */
    CHECK(pmm_get_free_block_count() > 100);
    CHECK(pmm_alloc_contiguous(2) == NULL);                           /* fragmented */

    /* Free a neighbour: now exactly one run of 3 exists */
    pmm_free_block((void*)held[1]);
    uint64_t run = (uint64_t)pmm_alloc_contiguous(3);
    CHECK(run == held[0]);                                            /* blocks 0,1,2 */

    /* Requests larger than the free count fail without changing state */
    uint64_t free_now = pmm_get_free_block_count();
    CHECK(pmm_alloc_contiguous(free_now + 1) == NULL);
    CHECK(pmm_get_free_block_count() == free_now);

    /* A fresh single block never lands inside the run just handed out */
    uint64_t first = (uint64_t)pmm_alloc_contiguous(1);
    CHECK(first != run && first != run + PMM_BLOCK_SIZE && first != run + 2 * PMM_BLOCK_SIZE);

    printf("pmm_test: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
