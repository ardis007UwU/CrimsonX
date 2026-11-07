#include "mm/vmm.h"
#include "mm/pmm.h"

// Extract 9-bit table indices from a virtual address
#define PML4_INDEX(vaddr) (((vaddr) >> 39) & 0x1FF)
#define PDP_INDEX(vaddr)  (((vaddr) >> 30) & 0x1FF)
#define PD_INDEX(vaddr)   (((vaddr) >> 21) & 0x1FF)
#define PT_INDEX(vaddr)   (((vaddr) >> 12) & 0x1FF)

// Mask out the bottom 12 bits to get the raw frame address
#define PAGE_FRAME(addr)  ((addr) & ~0xFFFull)

static uint64_t* pml4 = NULL;

static inline void load_cr3(uint64_t cr3) {
    asm volatile("mov %0, %%cr3" : : "r"(cr3));
}

// Retrieves the next page table level, allocating it via PMM if it doesn't exist
static uint64_t* get_next_level(uint64_t* current_level, uint64_t index, uint64_t flags) {
    if (!(current_level[index] & PAGE_PRESENT)) {
        // Allocate a new physical block for the table
        uint64_t* next_level = (uint64_t*)pmm_alloc_block();
        if (!next_level) return NULL;
        
        // Zero out the new table
        for (int i = 0; i < 512; i++) {
            next_level[i] = 0;
        }
        
        // Link it in the current directory
        current_level[index] = ((uint64_t)next_level) | flags | PAGE_PRESENT | PAGE_WRITE;
        return next_level;
    } else {
        return (uint64_t*)PAGE_FRAME(current_level[index]);
    }
}

void vmm_map_page(uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    if (!pml4) return;

    // Traverse down the 4 levels of x86_64 paging
    uint64_t* pdp = get_next_level(pml4, PML4_INDEX(vaddr), flags);
    if (!pdp) return;

    uint64_t* pd = get_next_level(pdp, PDP_INDEX(vaddr), flags);
    if (!pd) return;

    uint64_t* pt = get_next_level(pd, PD_INDEX(vaddr), flags);
    if (!pt) return;

    // Map the actual physical frame into the Page Table (Level 1)
    pt[PT_INDEX(vaddr)] = PAGE_FRAME(paddr) | flags | PAGE_PRESENT;
}

void vmm_init(void) {
    // 1. Allocate the top-level PML4 directory
    pml4 = (uint64_t*)pmm_alloc_block();
    for (int i = 0; i < 512; i++) {
        pml4[i] = 0;
    }

    // 2. Identity map the first 128 MB of RAM. 
    // This ensures our kernel code, variables, and the VGA buffer (0xB8000) 
    // don't suddenly disappear from memory when we flip the CR3 switch.
    for (uint64_t i = 0; i < (128ULL * 1024 * 1024); i += PAGE_SIZE) {
        vmm_map_page(i, i, PAGE_WRITE);
    }

    // 3. Load our new page tables into the CPU
    load_cr3((uint64_t)pml4);
}