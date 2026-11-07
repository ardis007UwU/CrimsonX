#include "mm/kmalloc.h"
#include "mm/pmm.h"

struct Header {
    size_t size;
    uint8_t is_free;
    struct Header* next;
};

static struct Header* head = NULL;

void kmalloc_init(void) {
    head = NULL;
}

void* kmalloc(size_t size) {
    if (size == 0) return NULL;

    size_t total_size = size + sizeof(struct Header);
    struct Header* curr = head;
    struct Header* prev = NULL;

    while (curr != NULL) {
        if (curr->is_free && curr->size >= size) {
            curr->is_free = 0;
            return (void*)(curr + 1);
        }
        prev = curr;
        curr = curr->next;
    }

    size_t blocks_needed = (total_size + PMM_BLOCK_SIZE - 1) / PMM_BLOCK_SIZE;
    void* block_mem = pmm_alloc_block();
    
    if (block_mem == NULL) return NULL;

    for (size_t i = 1; i < blocks_needed; i++) {
        pmm_alloc_block();
    }

    struct Header* new_header = (struct Header*)block_mem;
    new_header->size = (blocks_needed * PMM_BLOCK_SIZE) - sizeof(struct Header);
    new_header->is_free = 0;
    new_header->next = NULL;

    if (prev != NULL) {
        prev->next = new_header;
    } else {
        head = new_header;
    }

    return (void*)(new_header + 1);
}

void kfree(void* ptr) {
    if (ptr == NULL) return;

    struct Header* header = ((struct Header*)ptr) - 1;
    header->is_free = 1;

    struct Header* curr = head;
    while (curr != NULL) {
        // Merge adjacent free blocks to prevent fragmentation
        if (curr->is_free && curr->next != NULL && curr->next->is_free) {
            curr->size += sizeof(struct Header) + curr->next->size;
            curr->next = curr->next->next;
        } else {
            curr = curr->next;
        }
    }
}