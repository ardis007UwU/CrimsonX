#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE 4096

// x86_64 Page Table Entry Flags
#define PAGE_PRESENT (1ull << 0)
#define PAGE_WRITE   (1ull << 1)
#define PAGE_USER    (1ull << 2)

void vmm_init(void);
void vmm_map_page(uint64_t vaddr, uint64_t paddr, uint64_t flags);

#endif