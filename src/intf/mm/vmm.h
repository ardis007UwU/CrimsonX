#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE 4096

// x86_64 Page Table Entry Flags
#define PAGE_PRESENT (1ull << 0)
#define PAGE_WRITE   (1ull << 1)
#define PAGE_USER    (1ull << 2)
// OS-available PTE bit: the frame is not owned by the address space (shared or
// borrowed), so vmm_destroy_address_space() must not return it to the PMM.
#define PAGE_NO_FREE (1ull << 9)

/*
 * Address space layout
 *
 *   PML4 slot 0        kernel: identity map of the first 128 MB (supervisor-only).
 *                      Shared by every address space, so the kernel keeps
 *                      running after a CR3 switch.
 *   PML4 slots 1..255  user space [USER_SPACE_START, USER_SPACE_END). Private to
 *                      each process; every mapping here must carry PAGE_USER.
 *   PML4 slots 256+    unused (non-canonical hole / future higher half).
 *
 * Because user mappings never share a page-table page with the kernel
 * mapping, setting the U/S bit for user pages cannot expose kernel memory.
 */
#define USER_SPACE_START 0x0000008000000000ULL   /* 512 GiB */
#define USER_SPACE_END   0x0000800000000000ULL   /* end of lower canonical half */
#define USER_STACK_TOP   0x00007FFFFFFFF000ULL   /* page-aligned, grows down */

/** Initialize kernel page tables and activate the kernel address space. */
void vmm_init(void);

/** @return Physical address of the kernel PML4. */
uint64_t vmm_kernel_cr3(void);
/** @return Physical address of the currently tracked CR3. */
uint64_t vmm_current_cr3(void);

/** Load cr3 as the active address space; ignored for zero or current CR3. */
void vmm_switch(uint64_t cr3);

/** Create a user address space sharing supervisor mappings; returns zero on failure. */
uint64_t vmm_create_address_space(void);
/** Deep-copy every user mapping into a new address space; returns zero on failure. */
uint64_t vmm_clone_address_space(uint64_t source_cr3);

/** Destroy a user address space and return owned page frames to the allocator. */
void vmm_destroy_address_space(uint64_t cr3);

/**
 * Map one aligned physical page into cr3 with the requested permissions.
 * User virtual addresses require PAGE_USER and a non-kernel address space;
 * kernel mappings must remain supervisor-only.
 * @return 0 on success, or -1 for invalid, occupied, or out-of-range mappings.
 */
int vmm_map_page_in(uint64_t cr3, uint64_t vaddr, uint64_t paddr, uint64_t flags);

/**
 * Map one aligned physical page into the kernel address space.
 * The physical frame need not be managed by the PMM (for example, a PCI MMIO
 * BAR). Use PAGE_NO_FREE for borrowed/MMIO frames so teardown never frees them.
 */
int vmm_map_page(uint64_t vaddr, uint64_t paddr, uint64_t flags);

/** Remove one mapping and optionally free its physical frame. */
int vmm_unmap_page_in(uint64_t cr3, uint64_t vaddr, int free_frame);

/** Map zeroed user pages for size bytes; rolls back all pages on failure. */
int vmm_map_user_alloc(uint64_t cr3, uint64_t vaddr, size_t size, uint64_t flags);

/**
 * Walk the tables for `vaddr`. Returns 0 if mapped, -1 otherwise. *flags gets
 * the EFFECTIVE permissions: PAGE_PRESENT, plus PAGE_WRITE/PAGE_USER only if
 * every level grants them. *paddr (optional) gets the physical address
 * including the page offset.
 */
int vmm_query(uint64_t cr3, uint64_t vaddr, uint64_t* paddr, uint64_t* flags);

#endif
