/*
 * Host-side tests for src/impl/kernel/elf64.c (run with: make test-host)
 *
 * elf64.c only depends on the VMM (host-under-test-compatible) and kmalloc/kfree.
 * The PMM is faked exactly like tests/host/vmm_test.c, and the heap stubs route
 * through the system allocator so ASan catches any loader memory bug.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "kernel/elf64.h"

/* fake PMM: bounds-tracked aligned_alloc pages */
static long live_frames = 0;
static long base_frames = 0;

void* pmm_alloc_block(void) {
    void* p = aligned_alloc(4096, 4096);
    for (int i = 0; i < 4096; i++) ((uint8_t*)p)[i] = 0xAA;
    live_frames++;
    return p;
}

void pmm_free_block(void* ptr) {
    free(ptr);
    live_frames--;
}

void* kmalloc(size_t size) { return malloc(size); }
void kfree(void* p) { free(p); }

static void fill(uint8_t* dst, uint8_t byte, size_t n) {
    for (size_t i = 0; i < n; i++) dst[i] = byte;
}

static void wipe(void* p, size_t n) { fill((uint8_t*)p, 0, n); }

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { failures++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    else { printf("PASS %s\n", #cond); } \
} while (0)

#define IMG_BASE (USER_SPACE_START + 0x1000000ULL)   /* 16 MiB into user space */

/* ================================================================== */
/* Synthetic ELF builder                                              */
/* ================================================================== */

typedef struct {                 /* 64 bytes */
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
} ehdr_t;

typedef struct {                 /* 56 bytes */
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} phdr_t;

#define HDROFF   0x100          /* program image starts after headers */
#define TEXTSZ   0x180
#define DATASZ   0x200

static void put_ehdr(uint8_t* img, uint64_t entry, uint16_t phnum, uint16_t entsize) {
    ehdr_t* e = (ehdr_t*)img;
    wipe(e, sizeof(*e));
    e->e_ident[0] = 0x7f;
    e->e_ident[1] = 'E';
    e->e_ident[2] = 'L';
    e->e_ident[3] = 'F';
    e->e_ident[4] = 2;      /* 64-bit */
    e->e_ident[5] = 1;      /* LE */
    e->e_ident[6] = 1;      /* version */
    e->e_type = 2;
    e->e_machine = 62;
    e->e_version = 1;
    e->e_entry = entry;
    e->e_phoff = sizeof(ehdr_t);
    e->e_phentsize = entsize;
    e->e_phnum = phnum;
}

static void put_phdr(uint8_t* img, size_t idx, uint32_t type, uint32_t flags,
                     uint64_t offset, uint64_t vaddr, uint64_t filesz, uint64_t memsz) {
    phdr_t* p = (phdr_t*)(img + sizeof(ehdr_t) + idx * sizeof(phdr_t));
    wipe(p, sizeof(*p));
    p->p_type = type;
    p->p_flags = flags;
    p->p_offset = offset;
    p->p_vaddr = vaddr;
    p->p_paddr = 0;
    p->p_filesz = filesz;
    p->p_memsz = memsz;
    p->p_align = 0x1000;
}

static void test_two_segments(void) {
    static uint8_t img[2048];
    wipe(img, sizeof(img));

    /* payload bytes written over the image; we check them through the VMM */
    for (size_t i = 0; i < TEXTSZ; i++) img[HDROFF + i] = (uint8_t)(0x90 - i);       /* text */
    for (size_t i = 0; i < DATASZ; i++) img[HDROFF + TEXTSZ + i] = (uint8_t)(i * 7 + 3);

    uint64_t entry = IMG_BASE + 0x40;
    put_ehdr(img, entry, 2, sizeof(phdr_t));
    put_phdr(img, 0, 1 /*PT_LOAD*/, 5/*R|X*/, HDROFF, IMG_BASE, TEXTSZ, TEXTSZ + 0x20);
    put_phdr(img, 1, 1 /*PT_LOAD*/, 6/*R|W*/, HDROFF + TEXTSZ, IMG_BASE + 0x2000, DATASZ, DATASZ + 0x40);

    uint64_t cr3 = 0, e = 0, st = 0;
    CHECK(elf64_load(img, sizeof(img), &cr3, &e, &st) == 0);
    if (cr3) {
        CHECK(e == entry);
        CHECK(st == USER_STACK_TOP - 64);
        CHECK((st & 0xF) == 0);

        /* text bytes landed, bss tail is zeroed */
        uint64_t paddr = 0;
        CHECK(vmm_query(cr3, IMG_BASE + 7, &paddr, 0) == 0);
        CHECK(*(uint8_t*)paddr == (uint8_t)(0x90 - 7));
        CHECK(vmm_query(cr3, IMG_BASE + TEXTSZ, &paddr, 0) == 0);
        CHECK(*(uint8_t*)paddr == 0);                      /* memsz slack zeroed */

        /* data bytes landed */
        uint64_t da;
        CHECK(vmm_query(cr3, IMG_BASE + 0x2000 + 5, &da, 0) == 0);
        CHECK(*(uint8_t*)da == (uint8_t)(5 * 7 + 3));

        CHECK(vmm_query(cr3, USER_STACK_TOP - 0x1000, 0, 0) == 0);   /* stack mapped */
        CHECK(vmm_query(cr3, USER_STACK_TOP - (64 + 1) * 0x1000ULL, 0, 0) != 0);

        /* under the flags check: text must be non-writable, data writable */
        uint64_t tf = 0, df = 0;
        vmm_query(cr3, IMG_BASE, 0, &tf);
        vmm_query(cr3, IMG_BASE + 0x2000, 0, &df);
        CHECK((tf & PAGE_WRITE) == 0);
        CHECK((df & PAGE_WRITE) != 0);

        vmm_destroy_address_space(cr3);
        CHECK(live_frames == base_frames);
    }
}

static void test_shared_page_overlap(void) {
    static uint8_t img[2048];
    wipe(img, sizeof(img));

    /* two segments where the second starts mid-page of the first */
    const uint64_t v2 = IMG_BASE + 0x800;   /* inside first segment's page */
    for (size_t i = 0; i < TEXTSZ; i++) img[HDROFF + i] = 0xA1;
    for (size_t i = 0; i < 0x100; i++) img[HDROFF + TEXTSZ + i] = 0xB2;

    put_ehdr(img, IMG_BASE, 2, sizeof(phdr_t));
    put_phdr(img, 0, 1, 5, HDROFF, IMG_BASE, TEXTSZ, TEXTSZ);
    put_phdr(img, 1, 1, 6, HDROFF + TEXTSZ, v2, 0x100, 0x100);

    uint64_t cr3 = 0, e = 0, st = 0;
    CHECK(elf64_load(img, sizeof(img), &cr3, &e, &st) == 0);
    if (cr3) {
        uint64_t pa1 = 0, pa2 = 0;
        CHECK(vmm_query(cr3, IMG_BASE + 0x100, &pa1, 0) == 0);
        CHECK(vmm_query(cr3, v2 + 3, &pa2, 0) == 0);
        CHECK((pa1 & ~0xFFFULL) == (pa2 & ~0xFFFULL));      /* shared frame */
        CHECK(*(uint8_t*)(pa1 & ~0xFFFULL) == 0xA1);        /* seg1 survived */
        CHECK(*(uint8_t*)pa2 == 0xB2);        /* seg2 wrote */
        vmm_destroy_address_space(cr3);
        CHECK(live_frames == base_frames);
    }
}

static void test_rejections(void) {
    static uint8_t img[2048];
    wipe(img, sizeof(img));

    uint64_t cr3 = 0, e = 0, st = 0;
    CHECK(elf64_load(img, sizeof(img), &cr3, &e, &st) != 0);   /* no magic */

    /* valid image, mutated header fields */
    put_ehdr(img, IMG_BASE, 1, sizeof(phdr_t));
    put_phdr(img, 0, 1, 5, HDROFF, IMG_BASE, TEXTSZ, TEXTSZ);
    put_ehdr(img, IMG_BASE, 1, sizeof(phdr_t));   /* keep phdr intact */
    img[2] = 'X';                                               /* bad magic */
    CHECK(elf64_load(img, sizeof(img), &cr3, &e, &st) != 0);

    put_ehdr(img, IMG_BASE, 1, sizeof(phdr_t));
    ((ehdr_t*)img)->e_machine = 61;                             /* not AMD64 */
    CHECK(elf64_load(img, sizeof(img), &cr3, &e, &st) != 0);

    put_ehdr(img, IMG_BASE, 1, sizeof(phdr_t));
    img[5] = 2;                                                 /* big-endian */
    CHECK(elf64_load(img, sizeof(img), &cr3, &e, &st) != 0);

    put_ehdr(img, IMG_BASE, 1, 57);                             /* bad phentsize */
    CHECK(elf64_load(img, sizeof(img), &cr3, &e, &st) != 0);

    put_ehdr(img, IMG_BASE, 1, sizeof(phdr_t));
    put_phdr(img, 0, 1, 5, HDROFF, IMG_BASE, TEXTSZ, 200);      /* filesz > memsz */
    CHECK(elf64_load(img, sizeof(img), &cr3, &e, &st) != 0);

    /* file data past the image size */
    put_phdr(img, 0, 1, 5, 0x100000ULL, IMG_BASE, TEXTSZ, TEXTSZ);
    CHECK(elf64_load(img, sizeof(img), &cr3, &e, &st) != 0);

    /* image landing below USER_SPACE_START */
    put_phdr(img, 0, 1, 5, HDROFF, 0x400000ULL, TEXTSZ, TEXTSZ);
    CHECK(elf64_load(img, sizeof(img), &cr3, &e, &st) != 0);

    /* sanity: a fully valid image still loads (then must clean up) */
    put_phdr(img, 0, 1, 5, HDROFF, IMG_BASE, TEXTSZ, TEXTSZ);
    CHECK(elf64_load(img, sizeof(img), &cr3, &e, &st) == 0);
    if (cr3) vmm_destroy_address_space(cr3);
    CHECK(live_frames == base_frames);
}

int main(void) {
    vmm_init();
    base_frames = live_frames;
    test_two_segments();
    test_shared_page_overlap();
    test_rejections();

    printf("%s: %d checks, %d failures\n", "elf_test", checks, failures);
    return failures ? 1 : 0;
}
