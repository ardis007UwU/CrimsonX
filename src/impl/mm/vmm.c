#include "mm/vmm.h"
#include "mm/pmm.h"

// Extract 9-bit table indices from a virtual address
#define PML4_INDEX(vaddr) (((vaddr) >> 39) & 0x1FF)
#define PDP_INDEX(vaddr)  (((vaddr) >> 30) & 0x1FF)
#define PD_INDEX(vaddr)   (((vaddr) >> 21) & 0x1FF)
#define PT_INDEX(vaddr)   (((vaddr) >> 12) & 0x1FF)

// Physical frame address of an entry (bits 12..51), dropping flag bits
#define PAGE_FRAME(addr)  ((addr) & 0x000FFFFFFFFFF000ull)
// Flag bits we allow to be stored in a leaf entry
#define PTE_FLAG_MASK     0xFFFull
#define PTE_HUGE          (1ull << 7)

#define ENTRIES_PER_TABLE 512

static uint64_t* kernel_pml4 = NULL;
static uint64_t kernel_cr3 = 0;
static uint64_t current_cr3 = 0;

#ifdef VMM_HOST_TEST
/* Host-side unit tests (tests/host) run this file as a normal program where
 * "physical" addresses are plain pointers. No privileged instructions there. */
static inline void arch_load_cr3(uint64_t cr3) { (void)cr3; }
static inline void arch_invlpg(uint64_t vaddr)  { (void)vaddr; }
#else
static inline void arch_load_cr3(uint64_t cr3) {
    asm volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}
static inline void arch_invlpg(uint64_t vaddr) {
    asm volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
}
#endif

static void zero_table(uint64_t* table) {
    for (int i = 0; i < ENTRIES_PER_TABLE; i++) {
        table[i] = 0;
    }
}

// Allocate and zero one page-table page. Physical == virtual because the
// kernel identity-maps all RAM it manages.
static uint64_t* alloc_table(void) {
    uint64_t* table = (uint64_t*)pmm_alloc_block();
    if (table) zero_table(table);
    return table;
}

static int pml4_slot_is_user(uint64_t slot) {
    return slot >= PML4_INDEX(USER_SPACE_START) && slot < PML4_INDEX(USER_SPACE_END);
}

/*
 * Step down one level. When `create` is set, a missing table is allocated.
 *
 * The U/S bit must be set at EVERY level for ring 3 to access a page. An
 * existing intermediate entry that lacks it (created earlier by a
 * supervisor-only mapping) is upgraded here when a user mapping needs it;
 * previously it was left untouched, which made user pages fault.
 * Intermediate entries are always writable: the leaf decides write access.
 */
static uint64_t* get_next_level(uint64_t* table, uint64_t index, uint64_t flags, int create) {
    uint64_t entry = table[index];

    if (!(entry & PAGE_PRESENT)) {
        if (!create) return NULL;
        uint64_t* next = alloc_table();
        if (!next) return NULL;
        table[index] = ((uint64_t)next) | PAGE_PRESENT | PAGE_WRITE | (flags & PAGE_USER);
        return next;
    }

    if ((flags & PAGE_USER) && !(entry & PAGE_USER)) {
        table[index] = entry | PAGE_USER;
    }
    return (uint64_t*)PAGE_FRAME(entry);
}

// Kernel mappings live only in the kernel tables, user mappings only in
// process tables (never in the PML4 slot 0 subtree shared by all of them).
static int check_mapping(uint64_t cr3, uint64_t vaddr, uint64_t flags) {
    if (!cr3) return -1;
    if (vaddr & (PAGE_SIZE - 1)) return -1;
    if (vaddr >= USER_SPACE_END) return -1;

    int user_addr = vaddr >= USER_SPACE_START;
    int user_flag = (flags & PAGE_USER) != 0;
    if (user_addr != user_flag) return -1;
    if (user_addr == (cr3 == kernel_cr3)) return -1;
    return 0;
}

int vmm_map_page_in(uint64_t cr3, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    if (paddr & (PAGE_SIZE - 1)) return -1;
    if (check_mapping(cr3, vaddr, flags) != 0) return -1;

    uint64_t* pml4 = (uint64_t*)PAGE_FRAME(cr3);

    uint64_t* pdp = get_next_level(pml4, PML4_INDEX(vaddr), flags, 1);
    if (!pdp) return -1;

    uint64_t* pd = get_next_level(pdp, PDP_INDEX(vaddr), flags, 1);
    if (!pd) return -1;

    uint64_t* pt = get_next_level(pd, PD_INDEX(vaddr), flags, 1);
    if (!pt) return -1;

    if (pt[PT_INDEX(vaddr)] & PAGE_PRESENT) return -1;   // never silently remap

    pt[PT_INDEX(vaddr)] = PAGE_FRAME(paddr) | (flags & PTE_FLAG_MASK) | PAGE_PRESENT;

    if (cr3 == current_cr3) arch_invlpg(vaddr);
    return 0;
}

int vmm_map_page(uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    return vmm_map_page_in(kernel_cr3, vaddr, paddr, flags);
}

int vmm_unmap_page_in(uint64_t cr3, uint64_t vaddr, int free_frame) {
    if (!cr3 || (vaddr & (PAGE_SIZE - 1)) || vaddr >= USER_SPACE_END) return -1;

    uint64_t* pml4 = (uint64_t*)PAGE_FRAME(cr3);

    uint64_t* pdp = get_next_level(pml4, PML4_INDEX(vaddr), 0, 0);
    if (!pdp) return -1;
    uint64_t* pd = get_next_level(pdp, PDP_INDEX(vaddr), 0, 0);
    if (!pd) return -1;
    uint64_t* pt = get_next_level(pd, PD_INDEX(vaddr), 0, 0);
    if (!pt) return -1;

    uint64_t old = pt[PT_INDEX(vaddr)];
    if (!(old & PAGE_PRESENT)) return -1;

    pt[PT_INDEX(vaddr)] = 0;
    if (cr3 == current_cr3) arch_invlpg(vaddr);

    if (free_frame && !(old & PAGE_NO_FREE)) {
        pmm_free_block((void*)PAGE_FRAME(old));
    }
    return 0;
}

int vmm_map_user_alloc(uint64_t cr3, uint64_t vaddr, size_t size, uint64_t flags) {
    if (size == 0) return -1;

    // Round up without the (size + PAGE_SIZE - 1) form: that wraps to 0 pages
    // for sizes near UINT64_MAX and would "succeed" mapping nothing.
    uint64_t pages = (uint64_t)size / PAGE_SIZE + (((uint64_t)size % PAGE_SIZE) ? 1 : 0);
    // Reject wrap-around and ranges leaving user space
    if (pages > (USER_SPACE_END / PAGE_SIZE)) return -1;
    if (vaddr > USER_SPACE_END - pages * PAGE_SIZE) return -1;

    for (uint64_t i = 0; i < pages; i++) {
        uint64_t* frame = (uint64_t*)pmm_alloc_block();
        int ok = 0;

        if (frame) {
            zero_table(frame);   // a page is 512 qwords, same as a table
            ok = (vmm_map_page_in(cr3, vaddr + i * PAGE_SIZE, (uint64_t)frame, flags) == 0);
            if (!ok) pmm_free_block(frame);
        }

        if (!ok) {
            // Roll back everything mapped so far
            for (uint64_t j = 0; j < i; j++) {
                vmm_unmap_page_in(cr3, vaddr + j * PAGE_SIZE, 1);
            }
            return -1;
        }
    }
    return 0;
}

int vmm_query(uint64_t cr3, uint64_t vaddr, uint64_t* paddr, uint64_t* flags) {
    if (!cr3 || vaddr >= USER_SPACE_END) return -1;

    uint64_t index[4] = {
        PML4_INDEX(vaddr), PDP_INDEX(vaddr), PD_INDEX(vaddr), PT_INDEX(vaddr)
    };
    uint64_t* table = (uint64_t*)PAGE_FRAME(cr3);
    uint64_t effective = PAGE_WRITE | PAGE_USER;   // AND of every level

    for (int level = 0; level < 4; level++) {
        uint64_t entry = table[index[level]];
        if (!(entry & PAGE_PRESENT)) return -1;
        if (level < 3 && (entry & PTE_HUGE)) return -1;   // we never create huge pages

        effective &= entry;

        if (level == 3) {
            if (paddr) *paddr = PAGE_FRAME(entry) | (vaddr & (PAGE_SIZE - 1));
            if (flags) *flags = PAGE_PRESENT | (effective & (PAGE_WRITE | PAGE_USER));
            return 0;
        }
        table = (uint64_t*)PAGE_FRAME(entry);
    }
    return -1;
}

uint64_t vmm_kernel_cr3(void) { return kernel_cr3; }
uint64_t vmm_current_cr3(void) { return current_cr3; }

void vmm_switch(uint64_t cr3) {
    if (!cr3 || cr3 == current_cr3) return;
    arch_load_cr3(cr3);
    current_cr3 = cr3;
}

uint64_t vmm_create_address_space(void) {
    if (!kernel_pml4) return 0;

    uint64_t* pml4 = alloc_table();
    if (!pml4) return 0;

    // Share the kernel half by pointing at the same lower-level tables
    for (uint64_t i = 0; i < ENTRIES_PER_TABLE; i++) {
        if (!pml4_slot_is_user(i)) {
            pml4[i] = kernel_pml4[i];
        }
    }
    return (uint64_t)pml4;
}

void vmm_destroy_address_space(uint64_t cr3) {
    if (!cr3 || cr3 == kernel_cr3) return;

    // Never free the tables we are standing on
    if (cr3 == current_cr3) vmm_switch(kernel_cr3);

    uint64_t* l4 = (uint64_t*)PAGE_FRAME(cr3);

    for (uint64_t i = 0; i < ENTRIES_PER_TABLE; i++) {
        if (!pml4_slot_is_user(i) || !(l4[i] & PAGE_PRESENT)) continue;
        uint64_t* l3 = (uint64_t*)PAGE_FRAME(l4[i]);

        for (uint64_t j = 0; j < ENTRIES_PER_TABLE; j++) {
            if (!(l3[j] & PAGE_PRESENT)) continue;
            uint64_t* l2 = (uint64_t*)PAGE_FRAME(l3[j]);

            for (uint64_t k = 0; k < ENTRIES_PER_TABLE; k++) {
                if (!(l2[k] & PAGE_PRESENT)) continue;
                uint64_t* l1 = (uint64_t*)PAGE_FRAME(l2[k]);

                for (uint64_t m = 0; m < ENTRIES_PER_TABLE; m++) {
                    if ((l1[m] & PAGE_PRESENT) && !(l1[m] & PAGE_NO_FREE)) {
                        pmm_free_block((void*)PAGE_FRAME(l1[m]));
                    }
                }
                pmm_free_block(l1);
            }
            pmm_free_block(l2);
        }
        pmm_free_block(l3);
    }

    pmm_free_block(l4);
}

void vmm_init(void) {
    // 1. Allocate the top-level PML4 directory
    kernel_pml4 = alloc_table();
    kernel_cr3 = (uint64_t)kernel_pml4;

    // 2. Identity map the first 128 MB of RAM (supervisor-only).
    // This ensures our kernel code, variables, and the VGA buffer (0xB8000)
    // don't suddenly disappear from memory when we flip the CR3 switch.
    for (uint64_t i = 0; i < (128ULL * 1024 * 1024); i += PAGE_SIZE) {
        vmm_map_page(i, i, PAGE_WRITE);
    }

    // 3. Load our new page tables into the CPU
    arch_load_cr3(kernel_cr3);
    current_cr3 = kernel_cr3;
}
