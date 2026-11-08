#ifndef KERNEL_MMAP_H
#define KERNEL_MMAP_H

#include <stdint.h>
#include <stddef.h>
#include "fs/vfs.h"
#include "mm/vmm.h"

struct task;
typedef struct task task_t;

#define VM_MMAP_BASE  (USER_STACK_TOP - 0x01000000ULL)
#define VM_MMAP_LIMIT (USER_STACK_TOP - 64ULL * PAGE_SIZE - PAGE_SIZE)
#define VM_MMAP_MAX_SIZE (16ULL * 1024 * 1024)

/**
 * Map anonymous or private file-backed memory into a task address space.
 * Address zero selects a free range; a nonzero address is an exact placement.
 * Shared mappings are anonymous and retain their frames across fork().
 */
int vm_mmap(task_t* task, uint64_t address, size_t length, int protection,
            int flags, vfs_file_t* file, uint64_t offset, uint64_t* mapped_address);
/** Unmap one complete region previously created by vm_mmap(). */
int vm_munmap(task_t* task, uint64_t address, size_t length);
/** Copy mapping metadata and preserve shared mappings during fork. */
int vm_mmap_clone(task_t* parent, task_t* child);
/** Release shared mapping references owned by a task. */
void vm_mmap_destroy(task_t* task);

#endif
