#include "kernel/mmap.h"
#include "kernel/process.h"
#include "mm/kmalloc.h"
#include "mm/pmm.h"
#include "x86_64/syscall.h"

typedef struct {
    uint32_t references;
    size_t page_count;
    uint64_t* frames;
} vm_shared_object_t;

struct vm_mapping {
    uint64_t address;
    size_t length;
    uint64_t page_flags;
    int flags;
    vm_shared_object_t* shared;
    struct vm_mapping* next;
};

static int range_is_free(uint64_t cr3, uint64_t address, size_t length) {
    for (size_t offset = 0; offset < length; offset += PAGE_SIZE) {
        if (vmm_query(cr3, address + offset, NULL, NULL) == 0) return 0;
    }
    return 1;
}

static int find_free_range(uint64_t cr3, uint64_t begin, uint64_t end,
                           size_t length, uint64_t* address) {
    if (!address || begin >= end || length > end - begin) return -1;
    uint64_t last_start = end - length;
    for (uint64_t candidate = begin; candidate <= last_start;) {
        size_t offset = 0;
        while (offset < length &&
               vmm_query(cr3, candidate + offset, NULL, NULL) != 0) {
            offset += PAGE_SIZE;
        }
        if (offset == length) {
            *address = candidate;
            return 0;
        }
        candidate += offset + PAGE_SIZE;
    }
    return -1;
}

static void shared_release(vm_shared_object_t* object) {
    if (!object || object->references == 0) return;
    object->references--;
    if (object->references != 0) return;
    for (size_t i = 0; i < object->page_count; i++) {
        if (object->frames[i]) pmm_free_block((void*)(uintptr_t)object->frames[i]);
    }
    kfree(object->frames);
    kfree(object);
}

static void mapping_remove(task_t* task, struct vm_mapping* mapping) {
    struct vm_mapping** link = &task->mappings;
    while (*link && *link != mapping) link = &(*link)->next;
    if (*link == mapping) *link = mapping->next;
    shared_release(mapping->shared);
    kfree(mapping);
}

static int map_shared_object(task_t* task, struct vm_mapping* mapping) {
    vm_shared_object_t* object = (vm_shared_object_t*)kmalloc(sizeof(*object));
    if (!object) return -1;
    object->references = 1;
    object->page_count = mapping->length / PAGE_SIZE;
    object->frames = (uint64_t*)kmalloc(object->page_count * sizeof(uint64_t));
    if (!object->frames) {
        kfree(object);
        return -1;
    }
    for (size_t i = 0; i < object->page_count; i++) object->frames[i] = 0;

    size_t allocated = 0;
    for (; allocated < object->page_count; allocated++) {
        uint64_t* frame = (uint64_t*)pmm_alloc_block();
        if (!frame) break;
        uint8_t* bytes = (uint8_t*)frame;
        for (size_t j = 0; j < PAGE_SIZE; j++) bytes[j] = 0;
        object->frames[allocated] = (uint64_t)(uintptr_t)frame;
    }
    if (allocated != object->page_count) {
        object->page_count = allocated;
        object->references = 1;
        shared_release(object);
        return -1;
    }

    size_t mapped = 0;
    for (; mapped < object->page_count; mapped++) {
        uint64_t va = mapping->address + mapped * PAGE_SIZE;
        if (vmm_map_page_in(task->cr3, va, object->frames[mapped],
                            mapping->page_flags | PAGE_NO_FREE) != 0) break;
    }
    if (mapped != object->page_count) {
        for (size_t i = 0; i < mapped; i++) {
            vmm_unmap_page_in(task->cr3, mapping->address + i * PAGE_SIZE, 0);
        }
        shared_release(object);
        return -1;
    }

    mapping->shared = object;
    return 0;
}

static int copy_file_mapping(task_t* task, struct vm_mapping* mapping,
                             vfs_file_t* file, uint64_t offset, size_t length) {
    size_t original_offset = file->offset;
    if (original_offset > INT64_MAX || offset > INT64_MAX) return -1;
    if (vfs_file_seek(file, (int64_t)offset, VFS_SEEK_SET) != 0) return -1;

    uint8_t buffer[512];
    size_t copied = 0;
    int result = 0;
    while (copied < length) {
        size_t chunk = length - copied;
        if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
        int n = vfs_file_read(file, buffer, chunk);
        if (n < 0 || (size_t)n > chunk) {
            result = -1;
            break;
        }
        if (n == 0) break;

        for (size_t i = 0; i < (size_t)n; i++) {
            uint64_t physical = 0;
            if (vmm_query(task->cr3, mapping->address + copied + i,
                          &physical, NULL) != 0) {
                result = -1;
                break;
            }
            *(uint8_t*)(uintptr_t)physical = buffer[i];
        }
        if (result != 0) break;
        copied += (size_t)n;
    }

    if (vfs_file_seek(file, (int64_t)original_offset, VFS_SEEK_SET) != 0) result = -1;
    return result;
}

int vm_mmap(task_t* task, uint64_t address, size_t length, int protection,
            int flags, vfs_file_t* file, uint64_t offset, uint64_t* mapped_address) {
    const int allowed_flags = SYS_MAP_SHARED | SYS_MAP_PRIVATE | SYS_MAP_ANONYMOUS;
    if (!task || !task->cr3 || !mapped_address || length == 0 ||
        length > VM_MMAP_MAX_SIZE || (flags & ~allowed_flags) != 0 ||
        ((flags & (SYS_MAP_SHARED | SYS_MAP_PRIVATE)) != SYS_MAP_SHARED &&
         (flags & (SYS_MAP_SHARED | SYS_MAP_PRIVATE)) != SYS_MAP_PRIVATE) ||
        (protection & ~(SYS_PROT_READ | SYS_PROT_WRITE)) != 0 ||
        !(protection & SYS_PROT_READ) || (offset & (PAGE_SIZE - 1))) return -1;

    int anonymous = (flags & SYS_MAP_ANONYMOUS) != 0;
    int shared = (flags & SYS_MAP_SHARED) != 0;
    if (shared && !anonymous) return -1;
    if (!anonymous && (!file || !file->node ||
                       !(file->access_mode & VFS_ACCESS_READ) ||
                       file->node->type != VFS_TYPE_FILE)) return -1;

    size_t pages = length / PAGE_SIZE + ((length % PAGE_SIZE) ? 1 : 0);
    size_t rounded_length = pages * PAGE_SIZE;
    if (rounded_length < length) return -1;

    uint64_t start = address;
    if (start == 0) {
        if (find_free_range(task->cr3, task->mmap_base, VM_MMAP_LIMIT,
                            rounded_length, &start) != 0) return -1;
    } else if ((start & (PAGE_SIZE - 1)) || start < task->mmap_base ||
               start > VM_MMAP_LIMIT ||
               rounded_length > VM_MMAP_LIMIT - start ||
               !range_is_free(task->cr3, start, rounded_length)) {
        return -1;
    }

    struct vm_mapping* mapping = (struct vm_mapping*)kmalloc(sizeof(*mapping));
    if (!mapping) return -1;
    mapping->address = start;
    mapping->length = rounded_length;
    mapping->page_flags = PAGE_USER |
                          ((protection & SYS_PROT_WRITE) ? PAGE_WRITE : 0);
    mapping->flags = flags;
    mapping->shared = NULL;
    mapping->next = NULL;

    if (shared) {
        if (map_shared_object(task, mapping) != 0) {
            kfree(mapping);
            return -1;
        }
    } else {
        if (vmm_map_user_alloc(task->cr3, start, rounded_length,
                               mapping->page_flags) != 0) {
            kfree(mapping);
            return -1;
        }
        if (!anonymous && copy_file_mapping(task, mapping, file, offset, length) != 0) {
            for (size_t i = 0; i < pages; i++) {
                vmm_unmap_page_in(task->cr3, start + i * PAGE_SIZE, 1);
            }
            kfree(mapping);
            return -1;
        }
    }

    mapping->next = task->mappings;
    task->mappings = mapping;
    uint64_t next = start + rounded_length + PAGE_SIZE;
    if (next > task->mmap_next) task->mmap_next = next;
    *mapped_address = start;
    return 0;
}

int vm_munmap(task_t* task, uint64_t address, size_t length) {
    if (!task || length == 0 || (address & (PAGE_SIZE - 1))) return -1;
    size_t pages = length / PAGE_SIZE + ((length % PAGE_SIZE) ? 1 : 0);
    size_t rounded_length = pages * PAGE_SIZE;
    if (rounded_length < length) return -1;

    struct vm_mapping* mapping = task->mappings;
    while (mapping && (mapping->address != address || mapping->length != rounded_length)) {
        mapping = mapping->next;
    }
    if (!mapping) return -1;

    for (size_t i = 0; i < pages; i++) {
        if (vmm_query(task->cr3, address + i * PAGE_SIZE, NULL, NULL) != 0) return -1;
    }
    for (size_t i = 0; i < pages; i++) {
        if (vmm_unmap_page_in(task->cr3, address + i * PAGE_SIZE,
                              mapping->shared == NULL) != 0) return -1;
    }
    mapping_remove(task, mapping);
    return 0;
}

int vm_mmap_clone(task_t* parent, task_t* child) {
    if (!parent || !child) return -1;
    struct vm_mapping** tail = &child->mappings;
    for (struct vm_mapping* source = parent->mappings; source; source = source->next) {
        struct vm_mapping* copy = (struct vm_mapping*)kmalloc(sizeof(*copy));
        if (!copy) return -1;
        copy->address = source->address;
        copy->length = source->length;
        copy->page_flags = source->page_flags;
        copy->flags = source->flags;
        copy->shared = NULL;
        copy->next = NULL;

        if (source->shared) {
            size_t pages = source->length / PAGE_SIZE;
            size_t remapped = 0;
            for (; remapped < pages; remapped++) {
                uint64_t va = source->address + remapped * PAGE_SIZE;
                if (vmm_unmap_page_in(child->cr3, va, 1) != 0 ||
                    vmm_map_page_in(child->cr3, va,
                                    source->shared->frames[remapped],
                                    source->page_flags | PAGE_NO_FREE) != 0) break;
            }
            if (remapped != pages || source->shared->references == UINT32_MAX) {
                kfree(copy);
                return -1;
            }
            source->shared->references++;
            copy->shared = source->shared;
        }

        *tail = copy;
        tail = &copy->next;
    }
    return 0;
}

void vm_mmap_destroy(task_t* task) {
    if (!task) return;
    struct vm_mapping* mapping = task->mappings;
    task->mappings = NULL;
    while (mapping) {
        struct vm_mapping* next = mapping->next;
        shared_release(mapping->shared);
        kfree(mapping);
        mapping = next;
    }
}
