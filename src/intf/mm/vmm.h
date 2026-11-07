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

void vmm_init(void);

/* Physical address of the kernel's PML4 (what CR3 holds for kernel tasks). */
uint64_t vmm_kernel_cr3(void);
uint64_t vmm_current_cr3(void);

/* Load a new CR3 (no-op if it is already active). */
void vmm_switch(uint64_t cr3);

/* New address space with the kernel mapping shared and an empty user half.
 * Returns the PML4 physical address, or 0 if out of memory. */
uint64_t vmm_create_address_space(void);

/* Free all user mappings (and the frames they own) plus the table pages.
 * Switches back to the kernel address space first if it is the active one. */
void vmm_destroy_address_space(uint64_t cr3);

/*
 * Map one 4 KiB page into the address space `cr3`. Returns 0 on success, -1 if
 * rejected. Rejected: unaligned addresses, an address that is already mapped,
 * and anything that would mix kernel and user space:
 *   - user-range vaddr requires PAGE_USER and a non-kernel cr3
 *   - kernel-range vaddr forbids PAGE_USER and requires the kernel cr3
 * Intermediate page-table levels get the U bit as needed (x86 checks U/S at
 * every level).
 */
int vmm_map_page_in(uint64_t cr3, uint64_t vaddr, uint64_t paddr, uint64_t flags);

/* Kernel address space shorthand (used by vmm_init). */
int vmm_map_page(uint64_t vaddr, uint64_t paddr, uint64_t flags);

/* Remove one mapping; optionally return its frame to the PMM. */
int vmm_unmap_page_in(uint64_t cr3, uint64_t vaddr, int free_frame);

/* Allocate zeroed frames from the PMM and map `size` bytes at `vaddr`
 * (user space only). All-or-nothing: on failure nothing stays mapped. */
int vmm_map_user_alloc(uint64_t cr3, uint64_t vaddr, size_t size, uint64_t flags);

/*
 * Walk the tables for `vaddr`. Returns 0 if mapped, -1 otherwise. *flags gets
 * the EFFECTIVE permissions: PAGE_PRESENT, plus PAGE_WRITE/PAGE_USER only if
 * every level grants them. *paddr (optional) gets the physical address
 * including the page offset.
 */
int vmm_query(uint64_t cr3, uint64_t vaddr, uint64_t* paddr, uint64_t* flags);

#endif
