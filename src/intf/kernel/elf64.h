#ifndef ELF64_H
#define ELF64_H

#include <stdint.h>
#include <stddef.h>

#define ELF64_MAX_ARGC 32
#define ELF64_MAX_ENVC 32
#define ELF64_MAX_ARG_LENGTH 256

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
/**
 * Load a validated ELF64 executable into a new user address space.
 * @param image Kernel-readable ELF image.
 * @param size Image length in bytes.
 * @param out_cr3 Receives the new address-space root on success.
 * @param out_entry Receives the user entry point on success.
 * @param out_stack_top Receives the initial user stack pointer on success.
 * @return 0 on success; -1 on invalid input or allocation failure.
 */
int elf64_load(const void* image, size_t size, uint64_t* out_cr3,
               uint64_t* out_entry, uint64_t* out_stack_top);
/**
 * Load an ELF64 image and build its initial argc/argv/envp user stack.
 * @return 0 on success; -1 on invalid input or allocation failure.
 */
int elf64_load_args(const void* image, size_t size,
                    const char* const argv[], size_t argc,
                    const char* const envp[], size_t envc,
                    uint64_t* out_cr3, uint64_t* out_entry,
                    uint64_t* out_stack_top, uint64_t* out_heap_start);

#endif
