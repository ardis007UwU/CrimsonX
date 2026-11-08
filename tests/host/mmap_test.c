#include "kernel/mmap.h"
#include "kernel/process.h"
#include "x86_64/syscall.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_MAP_CAPACITY 128

typedef struct {
    uint64_t cr3;
    uint64_t address;
    uint64_t physical;
    uint64_t flags;
    int used;
} test_mapping_t;

static test_mapping_t mappings[TEST_MAP_CAPACITY];
static int checks;
static int failures;
static int kernel_allocations;
static int kernel_releases;
static int frame_allocations;
static int frame_releases;

#define CHECK(expression) do { \
    checks++; \
    if (!(expression)) { \
        failures++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
    } \
} while (0)

void* kmalloc(size_t size) {
    void* memory = malloc(size);
    if (memory) kernel_allocations++;
    return memory;
}

void kfree(void* memory) {
    if (!memory) return;
    kernel_releases++;
    free(memory);
}

void* pmm_alloc_block(void) {
    void* frame = aligned_alloc(PAGE_SIZE, PAGE_SIZE);
    if (frame) {
        memset(frame, 0, PAGE_SIZE);
        frame_allocations++;
    }
    return frame;
}

void pmm_free_block(void* frame) {
    if (!frame) return;
    frame_releases++;
    free(frame);
}

int vfs_file_seek(vfs_file_t* file, int64_t offset, int whence) {
    if (!file || !file->node) return -1;
    uint64_t base = 0;
    if (whence == VFS_SEEK_CUR) base = file->offset;
    else if (whence == VFS_SEEK_END) base = file->node->size;
    else if (whence != VFS_SEEK_SET) return -1;

    uint64_t target = base;
    if (offset < 0) {
        uint64_t amount = 0 - (uint64_t)offset;
        if (amount > base) return -1;
        target -= amount;
    } else {
        if ((uint64_t)offset > SIZE_MAX - base) return -1;
        target += (uint64_t)offset;
    }
    file->offset = (size_t)target;
    return 0;
}

int vfs_file_read(vfs_file_t* file, void* buffer, size_t count) {
    if (!file || !file->node || !buffer || !file->node->data) return -1;
    if (file->offset >= file->node->size) return 0;
    size_t available = file->node->size - file->offset;
    size_t moved = count < available ? count : available;
    memcpy(buffer, (uint8_t*)file->node->data + file->offset, moved);
    file->offset += moved;
    return (int)moved;
}

static test_mapping_t* find_mapping(uint64_t cr3, uint64_t address) {
    uint64_t page = address & ~(uint64_t)(PAGE_SIZE - 1);
    for (size_t i = 0; i < TEST_MAP_CAPACITY; i++) {
        if (mappings[i].used && mappings[i].cr3 == cr3 &&
            mappings[i].address == page) return &mappings[i];
    }
    return NULL;
}

int vmm_query(uint64_t cr3, uint64_t address, uint64_t* physical, uint64_t* flags) {
    test_mapping_t* mapping = find_mapping(cr3, address);
    if (!mapping) return -1;
    if (physical) *physical = mapping->physical + (address & (PAGE_SIZE - 1));
    if (flags) *flags = mapping->flags | PAGE_PRESENT;
    return 0;
}

int vmm_map_page_in(uint64_t cr3, uint64_t address, uint64_t physical, uint64_t flags) {
    if (find_mapping(cr3, address)) return -1;
    for (size_t i = 0; i < TEST_MAP_CAPACITY; i++) {
        if (!mappings[i].used) {
            mappings[i].cr3 = cr3;
            mappings[i].address = address;
            mappings[i].physical = physical;
            mappings[i].flags = flags;
            mappings[i].used = 1;
            return 0;
        }
    }
    return -1;
}

int vmm_unmap_page_in(uint64_t cr3, uint64_t address, int free_frame) {
    test_mapping_t* mapping = find_mapping(cr3, address);
    if (!mapping || (address & (PAGE_SIZE - 1))) return -1;
    if (free_frame && !(mapping->flags & PAGE_NO_FREE)) {
        pmm_free_block((void*)(uintptr_t)mapping->physical);
    }
    mapping->used = 0;
    return 0;
}

int vmm_map_user_alloc(uint64_t cr3, uint64_t address, size_t size, uint64_t flags) {
    size_t pages = size / PAGE_SIZE + ((size % PAGE_SIZE) ? 1 : 0);
    for (size_t i = 0; i < pages; i++) {
        void* frame = pmm_alloc_block();
        if (!frame || vmm_map_page_in(cr3, address + i * PAGE_SIZE,
                                     (uint64_t)(uintptr_t)frame, flags) != 0) {
            if (frame) pmm_free_block(frame);
            while (i > 0) {
                i--;
                vmm_unmap_page_in(cr3, address + i * PAGE_SIZE, 1);
            }
            return -1;
        }
    }
    return 0;
}

static void test_private_mapping(void) {
    task_t task = { 0 };
    task.cr3 = 1;
    task.mmap_base = VM_MMAP_BASE;
    task.mmap_next = VM_MMAP_BASE;
    uint64_t address = 0;

    CHECK(vm_mmap(&task, 0, PAGE_SIZE + 1, SYS_PROT_READ | SYS_PROT_WRITE,
                  SYS_MAP_PRIVATE | SYS_MAP_ANONYMOUS, NULL, 0, &address) == 0);
    CHECK(address == VM_MMAP_BASE);
    CHECK(vmm_query(task.cr3, address + PAGE_SIZE, NULL, NULL) == 0);
    CHECK(vm_munmap(&task, address, PAGE_SIZE) != 0);
    CHECK(vm_munmap(&task, address, PAGE_SIZE + 1) == 0);
    CHECK(vmm_query(task.cr3, address, NULL, NULL) != 0);
    vm_mmap_destroy(&task);
}

static void test_shared_mapping_fork(void) {
    task_t parent = { 0 };
    task_t child = { 0 };
    parent.cr3 = 2;
    parent.mmap_base = VM_MMAP_BASE;
    parent.mmap_next = VM_MMAP_BASE;
    child.cr3 = 3;
    child.mmap_base = VM_MMAP_BASE;

    uint64_t address = 0;
    CHECK(vm_mmap(&parent, 0, PAGE_SIZE, SYS_PROT_READ | SYS_PROT_WRITE,
                  SYS_MAP_SHARED | SYS_MAP_ANONYMOUS, NULL, 0, &address) == 0);
    CHECK(vmm_map_user_alloc(child.cr3, address, PAGE_SIZE,
                             PAGE_USER | PAGE_WRITE) == 0);
    CHECK(vm_mmap_clone(&parent, &child) == 0);

    uint64_t parent_physical = 0;
    uint64_t child_physical = 0;
    CHECK(vmm_query(parent.cr3, address, &parent_physical, NULL) == 0);
    CHECK(vmm_query(child.cr3, address, &child_physical, NULL) == 0);
    CHECK((parent_physical & ~(uint64_t)(PAGE_SIZE - 1)) ==
          (child_physical & ~(uint64_t)(PAGE_SIZE - 1)));
    *(uint8_t*)(uintptr_t)parent_physical = 0xA5;
    CHECK(*(uint8_t*)(uintptr_t)child_physical == 0xA5);

    CHECK(vm_munmap(&parent, address, PAGE_SIZE) == 0);
    CHECK(vmm_query(child.cr3, address, &child_physical, NULL) == 0);
    CHECK(*(uint8_t*)(uintptr_t)child_physical == 0xA5);
    CHECK(vm_munmap(&child, address, PAGE_SIZE) == 0);
    vm_mmap_destroy(&parent);
    vm_mmap_destroy(&child);
}

static void test_private_file_mapping(void) {
    uint8_t contents[PAGE_SIZE + 32];
    for (size_t i = 0; i < sizeof(contents); i++) contents[i] = (uint8_t)i;
    vfs_node_t node = { 0 };
    node.type = VFS_TYPE_FILE;
    node.size = sizeof(contents);
    node.data = contents;
    vfs_file_t file = { 0 };
    file.node = &node;
    file.offset = 7;
    file.access_mode = VFS_ACCESS_READ;

    task_t task = { 0 };
    task.cr3 = 4;
    task.mmap_base = VM_MMAP_BASE;
    task.mmap_next = VM_MMAP_BASE;
    uint64_t address = 0;
    CHECK(vm_mmap(&task, 0, sizeof(contents), SYS_PROT_READ,
                  SYS_MAP_PRIVATE, &file, PAGE_SIZE, &address) == 0);
    CHECK(file.offset == 7);

    uint64_t physical = 0;
    CHECK(vmm_query(task.cr3, address, &physical, NULL) == 0);
    CHECK(*(uint8_t*)(uintptr_t)physical == contents[PAGE_SIZE]);
    CHECK(vmm_query(task.cr3, address + 31, &physical, NULL) == 0);
    CHECK(*(uint8_t*)(uintptr_t)physical == contents[PAGE_SIZE + 31]);
    CHECK(vmm_query(task.cr3, address + sizeof(contents), &physical, NULL) == 0);
    CHECK(*(uint8_t*)(uintptr_t)physical == 0);
    CHECK(vm_munmap(&task, address, sizeof(contents)) == 0);
    vm_mmap_destroy(&task);
}

int main(void) {
    test_private_mapping();
    test_shared_mapping_fork();
    test_private_file_mapping();
    CHECK(kernel_allocations == kernel_releases);
    CHECK(frame_allocations == frame_releases);
    printf("mmap_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
