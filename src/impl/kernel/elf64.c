#include "kernel/elf64.h"
#include "mm/vmm.h"
#include "string.h"
#include "mm/kmalloc.h"
#include "kernel/mmap.h"

/* ---- on-image structures (the format's little-endian fields map 1:1) ---- */

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf64_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} elf64_phdr_t;

#define ELF_MAGIC0 0x7F
#define ELF_MAGIC1 'E'
#define ELF_MAGIC2 'L'
#define ELF_MAGIC3 'F'
#define ELF_CLASS_64      2
#define ELF_DATA_LSB      1
#define ELF_MACH_AMD64    62
#define ELF_ET_EXEC       2

#define PT_LOAD           1
#define PF_X 1
#define PF_W 2
#define PF_R 4

/* User stack: 64 pages (256 KiB) below USER_STACK_TOP */
#define ELF_STACK_PAGES 64

_Static_assert(sizeof(elf64_ehdr_t) == 64, "ELF64 Ehdr must be 64 bytes");
_Static_assert(sizeof(elf64_phdr_t) == 56, "ELF64 Phdr must be 56 bytes");

static int safe_read(const uint8_t* img, size_t size, size_t off, void* out, size_t len) {
    if (off > size || len > size - off) return -1;
    unsigned char* dst = (unsigned char*)out;
    const unsigned char* src = img + off;
    for (size_t i = 0; i < len; i++) dst[i] = src[i];
    return 0;
}

/*
 * Make sure virt..virt+PAGE_SIZE is mapped in cr3 and return the frame that
 * backs it via the kernel identity map. Allocates a zeroed page when needed,
 * so updating an already-mapped page (segment overlap) is just a no-op map.
 */
static int map_and_get_frame(uint64_t cr3, uint64_t virt, uint64_t flags, uint8_t** frame) {
    uint64_t paddr = 0;
    if (vmm_query(cr3, virt, &paddr, 0) == 0) {
        *frame = (uint8_t*)(paddr & ~0xFFFULL);
        return 0;
    }
    if (vmm_map_user_alloc(cr3, virt, PAGE_SIZE, flags) != 0) return -1;
    if (vmm_query(cr3, virt, &paddr, 0) != 0) return -1;
    *frame = (uint8_t*)(paddr & ~0xFFFULL);
    return 0;
}

/* Copy `len` file bytes starting at img[file] to user memory at [virt].
 * `flags` only matters for pages this call maps itself (they should not
 * exist — the segment mapping loop covers them first — but be correct). */
static int copy_to_user_pages(uint64_t cr3, uint64_t virt, uint64_t flags,
                              const uint8_t* img, size_t file, size_t len) {
    while (len > 0) {
        uint64_t page = virt & ~0xFFFULL;
        size_t in_page = (size_t)(virt - page);
        size_t chunk = PAGE_SIZE - in_page;
        if (chunk > len) chunk = len;

        uint8_t* frame;
        if (map_and_get_frame(cr3, page, flags, &frame) != 0) return -1;
        /* A page already mapped by an earlier segment keeps that segment's
         * permissions: overlap shares the physical frame. */
        uint8_t* dst = frame + in_page;
        const uint8_t* src = img + file;
        for (size_t i = 0; i < chunk; i++) dst[i] = src[i];

        virt += chunk;
        file += chunk;
        len -= chunk;
    }
    return 0;
}

static int push_stack_string(uint64_t cr3, uint64_t stack_lo, uint64_t* stack,
                             const char* text, uint64_t* user_pointer) {
    if (!text || !stack || !user_pointer) return -1;
    size_t length = 0;
    while (length < ELF64_MAX_ARG_LENGTH && text[length] != '\0') length++;
    if (length == ELF64_MAX_ARG_LENGTH) return -1;
    length++;
    if (*stack < stack_lo || length > *stack - stack_lo) return -1;

    *stack -= length;
    if (copy_to_user_pages(cr3, *stack, PAGE_USER | PAGE_WRITE,
                           (const uint8_t*)text, 0, length) != 0) return -1;
    *user_pointer = *stack;
    return 0;
}

static int build_initial_stack(uint64_t cr3, uint64_t stack_top,
                               const char* const argv[], size_t argc,
                               const char* const envp[], size_t envc,
                               uint64_t* out_stack) {
    if (argc > ELF64_MAX_ARGC || envc > ELF64_MAX_ENVC ||
        (argc && !argv) || (envc && !envp)) return -1;

    uint64_t argv_user[ELF64_MAX_ARGC];
    uint64_t envp_user[ELF64_MAX_ENVC];
    uint64_t stack = stack_top;
    uint64_t stack_lo = stack_top - ELF_STACK_PAGES * PAGE_SIZE;

    for (size_t i = envc; i > 0; i--) {
        if (push_stack_string(cr3, stack_lo, &stack, envp[i - 1],
                              &envp_user[i - 1]) != 0) return -1;
    }
    for (size_t i = argc; i > 0; i--) {
        if (push_stack_string(cr3, stack_lo, &stack, argv[i - 1],
                              &argv_user[i - 1]) != 0) return -1;
    }

    size_t word_count = 1 + argc + 1 + envc + 1;
    size_t table_size = word_count * sizeof(uint64_t);
    if (stack < stack_lo || table_size > stack - stack_lo) return -1;
    uint64_t table_start = (stack - table_size) & ~0xFULL;
    if (table_start < stack_lo) return -1;

    uint64_t table[1 + ELF64_MAX_ARGC + 1 + ELF64_MAX_ENVC + 1];
    size_t at = 0;
    table[at++] = argc;
    for (size_t i = 0; i < argc; i++) table[at++] = argv_user[i];
    table[at++] = 0;
    for (size_t i = 0; i < envc; i++) table[at++] = envp_user[i];
    table[at++] = 0;

    if (copy_to_user_pages(cr3, table_start, PAGE_USER | PAGE_WRITE,
                           (const uint8_t*)table, 0, table_size) != 0) return -1;
    *out_stack = table_start;
    return 0;
}

int elf64_load_args(const void* raw, size_t size,
                    const char* const argv[], size_t argc,
                    const char* const envp[], size_t envc,
                    uint64_t* out_cr3, uint64_t* out_entry,
                    uint64_t* out_stack_top, uint64_t* out_heap_start) {
    const uint8_t* img = (const uint8_t*)raw;
    elf64_ehdr_t ehdr;
    elf64_phdr_t* phdrs = NULL;

    if (!img || !out_cr3 || !out_entry || !out_stack_top ||
        argc > ELF64_MAX_ARGC || envc > ELF64_MAX_ENVC ||
        (argc && !argv) || (envc && !envp) ||
        !out_heap_start || size < sizeof(elf64_ehdr_t)) return -1;

    if (img[0] != ELF_MAGIC0 || img[1] != ELF_MAGIC1 ||
        img[2] != ELF_MAGIC2 || img[3] != ELF_MAGIC3) return -1;
    if (img[4] != ELF_CLASS_64 || img[5] != ELF_DATA_LSB) return -1;
    if (img[6] != 1) return -1;   /* EV_CURRENT */

    if (safe_read(img, size, 0, &ehdr, sizeof(ehdr)) != 0) return -1;
    if (ehdr.e_machine != ELF_MACH_AMD64 || ehdr.e_version != 1) return -1;
    if (ehdr.e_phentsize != sizeof(elf64_phdr_t)) return -1;
    if (ehdr.e_phnum == 0) return -1;
    if (ehdr.e_type != ELF_ET_EXEC) return -1;

    size_t phbytes = (size_t)ehdr.e_phnum * sizeof(elf64_phdr_t);
    if (size < ehdr.e_phoff || phbytes > size - ehdr.e_phoff) return -1;

    phdrs = (elf64_phdr_t*)kmalloc(phbytes);
    if (!phdrs) return -1;
    if (safe_read(img, size, ehdr.e_phoff, phdrs, phbytes) != 0) {
        kfree(phdrs);
        return -1;
    }

    uint64_t highest_segment_end = USER_SPACE_START;
    /* Pre-flight: validate every PT_LOAD before mapping anything. */
    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        elf64_phdr_t* p = &phdrs[i];
        if (p->p_type != PT_LOAD) continue;
        if (p->p_memsz == 0) continue;
        if (p->p_filesz > p->p_memsz) { kfree(phdrs); return -1; }
        if (size < (size_t)p->p_offset ||
            p->p_filesz > size - (size_t)p->p_offset) {
            kfree(phdrs);
            return -1;
        }
        uint64_t lo = p->p_vaddr & ~0xFFFULL;
        if (lo < USER_SPACE_START) { kfree(phdrs); return -1; }
        uint64_t hi = (p->p_vaddr + p->p_memsz + PAGE_SIZE - 1) & ~0xFFFULL;
        if (hi > USER_SPACE_END || hi < p->p_vaddr || hi < lo) { kfree(phdrs); return -1; }
        if (hi > highest_segment_end) highest_segment_end = hi;
    }
    if (highest_segment_end > VM_MMAP_BASE) { kfree(phdrs); return -1; }

    uint64_t cr3 = vmm_create_address_space();
    if (!cr3) { kfree(phdrs); return -1; }

    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        elf64_phdr_t* p = &phdrs[i];
        if (p->p_type != PT_LOAD || p->p_memsz == 0) continue;

        uint64_t flags = PAGE_USER;
        if (p->p_flags & PF_W) flags |= PAGE_WRITE;

        uint64_t start = p->p_vaddr & ~0xFFFULL;
        uint64_t end = (p->p_vaddr + p->p_memsz + PAGE_SIZE - 1) & ~0xFFFULL;

        for (uint64_t page = start; page < end; page += PAGE_SIZE) {
            uint8_t* frame;
            if (map_and_get_frame(cr3, page, flags, &frame) != 0) {
                goto fail;
            }
        }

        /* File content lands at the segment's file offset; everything beyond
         * p_filesz (BSS) stays zero because mapping zeroed the pages. */
        if (p->p_filesz > 0) {
            if (copy_to_user_pages(cr3, p->p_vaddr, flags, img, p->p_offset, p->p_filesz) != 0) {
                goto fail;
            }
        }
    }

    /* Stack: ELF_STACK_PAGES pages ending at USER_STACK_TOP. The first
     * (lowest) address is half of the guard-free region; ustack_top handed
     * to user mode is page-aligned and 16-byte spaced for callee pushes. */
    uint64_t stack_top = USER_STACK_TOP;
    uint64_t stack_lo = stack_top - ELF_STACK_PAGES * PAGE_SIZE;
    if (vmm_map_user_alloc(cr3, stack_lo, ELF_STACK_PAGES * PAGE_SIZE,
                           PAGE_USER | PAGE_WRITE) != 0) {
        goto fail;
    }

    uint64_t initial_stack;
    if (build_initial_stack(cr3, stack_top, argv, argc, envp, envc,
                            &initial_stack) != 0) goto fail;
    if (vmm_query(cr3, ehdr.e_entry, NULL, NULL) != 0) goto fail;

    kfree(phdrs);
    *out_cr3 = cr3;
    *out_entry = ehdr.e_entry;
    *out_stack_top = initial_stack;
    *out_heap_start = highest_segment_end;
    return 0;

fail:
    kfree(phdrs);
    vmm_destroy_address_space(cr3);
    return -1;
}

int elf64_load(const void* raw, size_t size, uint64_t* out_cr3,
               uint64_t* out_entry, uint64_t* out_stack_top) {
    uint64_t heap_start = 0;
    return elf64_load_args(raw, size, NULL, 0, NULL, 0,
                           out_cr3, out_entry, out_stack_top, &heap_start);
}
