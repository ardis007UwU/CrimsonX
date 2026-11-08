/*
 * execve: read an ELF image through the VFS, build its user address space
 * with elf64_load, then transition the current task into ring 3 in one
 * uninterrupted step (exec_drop, implemented in syscall_entry.asm).
 *
 * The drop must be atomic because the syscall handler ran with interrupts
 * masked; exec_drop first moves us to the task's kernel stack, then frees
 * the exiting address space, loads the new CR3 via vmm_switch and iretqs
 * straight into user code.
 */
#include "kernel/exec.h"
#include "kernel/elf64.h"
#include "kernel/mmap.h"
#include "fs/vfs.h"
#include "fs/crimsonfs.h"
#include "mm/vmm.h"
#include "mm/kmalloc.h"
#include "string.h"

extern void exec_drop(uint64_t old_cr3, uint64_t new_cr3,
                      void* entry, void* ustack, void* kstack_top);

#define EXEC_MAX_IMAGE (4 * 1024 * 1024)   /* sanity cap for any image */
#define EXEC_MAX_PATH 128

static int exec_replace_current_internal(task_t* task, const char* path,
                                         const char* const argv[], size_t argc,
                                         const char* const envp[], size_t envc,
                                         void** owned_args) {
    if (!task || !task->kernel_stack_top) return -1;
    if (!path || path[0] == '\0') return -1;

    vfs_node_t* node = vfs_open(path);
    if (!node) return -1;
    if (node->type != VFS_TYPE_FILE) {
        vfs_close(node);
        return -1;
    }
    if (node->size == 0 || node->size > EXEC_MAX_IMAGE || !node->read) {
        vfs_close(node);
        return -1;
    }

    size_t want = node->size;

    uint8_t* image = (uint8_t*)kmalloc(want);
    if (!image) {
        vfs_close(node);
        return -1;
    }

    size_t got = 0;
    while (got < want) {
        int n = node->read(node, image + got, want - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    vfs_close(node);

    if (got != want) {
        kfree(image);
        return -1;
    }

    uint64_t entry = 0, ustack = 0, new_cr3 = 0, heap_start = 0;
    if (elf64_load_args(image, got, argv, argc, envp, envc,
                        &new_cr3, &entry, &ustack, &heap_start) != 0) {
        kfree(image);
        return -1;
    }
    kfree(image);
    if (owned_args && *owned_args) {
        kfree(*owned_args);
        *owned_args = NULL;
    }
    vm_mmap_destroy(task);

    /* Release heap-backed descriptors; channels (stdin/stdout) survive. */
    for (int i = 0; i < VFS_FD_MAX; i++) {
        vfs_file_t* f = task->fds[i];
        if (f && f->owns_node) {
            vfs_file_release(f);
            task->fds[i] = NULL;
        }
    }

    /* Commit the swap. */
    uint64_t old_cr3 = task->cr3;
    task->user_entry = entry;
    task->user_stack = ustack;
    task->cr3 = new_cr3;
    task->heap_start = heap_start;
    task->heap_end = heap_start;
    task->heap_limit = VM_MMAP_BASE;
    task->mmap_base = VM_MMAP_BASE;
    task->mmap_next = VM_MMAP_BASE;

    exec_drop(old_cr3, new_cr3, (void*)(uintptr_t)entry,
              (void*)(uintptr_t)ustack, (void*)task->kernel_stack_top);

    /* exec_drop never returns; reaching here is a kernel bug. */
    return -1;
}

int exec_replace_current_args(task_t* task, const char* path,
                              const char* const argv[], size_t argc,
                              const char* const envp[], size_t envc) {
    return exec_replace_current_internal(task, path, argv, argc, envp, envc, NULL);
}

int exec_replace_current_args_owned(task_t* task, const char* path,
                                    const char* const argv[], size_t argc,
                                    const char* const envp[], size_t envc,
                                    void** owned_args) {
    return exec_replace_current_internal(task, path, argv, argc, envp, envc,
                                         owned_args);
}

int exec_replace_current(task_t* task, const char* path) {
    const char* argv[1] = { path };
    return exec_replace_current_args(task, path, argv, 1, NULL, 0);
}

/* ---------------- spawn path (shell entry) ---------------- */

static void spawn_runner(void) {
    task_t* self = process_current_task();
    const char* path = (const char*)self->spawn_arg;

    /* Success drops into ring 3 and never returns; failure (or an
     * impossible exec_drop return) falls through to orderly suicide. */
    exec_replace_current(self, path);
    process_kill_current(127);
}

task_t* process_spawn_user(const char* path) {
    if (!path || path[0] == '\0') return NULL;
    size_t path_len = 0;
    while (path_len <= EXEC_MAX_PATH && path[path_len] != '\0') path_len++;
    if (path_len > EXEC_MAX_PATH) return NULL;

    uint8_t* p = (uint8_t*)kmalloc(EXEC_MAX_PATH + 1);
    if (!p) return NULL;

    for (size_t i = 0; i <= path_len; i++) p[i] = (uint8_t)path[i];

    task_t* task = process_create(spawn_runner);
    if (!task) {
        kfree(p);
        return NULL;
    }
    task->spawn_arg = p;
    return task;
}
