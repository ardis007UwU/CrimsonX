#ifndef ELF64_H
#define ELF64_H

#include <stdint.h>
#include <stddef.h>

/*
 * ELF64 loader.
 *
 * Parses an ELF image already present in kernel memory (read from disk by the
 * VFS layer), creates a fresh user address space with vmm_create_address_space
 * and maps every PT_LOAD segment into the active user range using
 * vmm_map_user_alloc(). File bytes are copied through the kernel identity map
 * (user frames live below the identity-mapped 128 MB and are physically
 * writable from the kernel). Pages shared by adjacent segments (the classic
 * text/data boundary page) are mapped once and shared.
 *
 * On success *out_cr3 receives the new PML4 (caller owns it and is responsible
 * for vmm_destroy_address_space on process death), *out_entry the ELF entry
 * point and *out_stack_top the (page-aligned, 16-byte clean) user stack top.
 */
int elf64_load(const void* image, size_t size, uint64_t* out_cr3,
               uint64_t* out_entry, uint64_t* out_stack_top);

#endif
