/*
 * Host-side tests for src/impl/mm/vmm.c  (run with: make test-host)
 *
 * vmm.c is compiled with -DVMM_HOST_TEST, which turns CR3 loads and invlpg into
 * no-ops. "Physical" memory is plain malloc'd pages, so a physical address is a
 * pointer exactly like the identity-mapped kernel. The PMM is faked so frame
 * leaks and allocation failures can be injected.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "mm/vmm.h"
#include "mm/pmm.h"

static long live_frames = 0;
static long budget = -1;      /* >= 0: allocations left before the fake PMM fails */

void* pmm_alloc_block(void) {
    if (budget == 0) return NULL;
    if (budget > 0) budget--;
    void* p = aligned_alloc(4096, 4096);
    /* garbage, so the code under test must zero what it needs. (Plain loop: with
     * -I src/intf the project's own string.h shadows <string.h>.) */
    for (int i = 0; i < 4096; i++) ((uint8_t*)p)[i] = 0xAA;
    live_frames++;
    return p;
}

void pmm_free_block(void* ptr) {
    free(ptr);
    live_frames--;
}

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { failures++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define P_W_U (PAGE_PRESENT | PAGE_WRITE | PAGE_USER)
#define UVA   (USER_SPACE_START + 0x400000ULL)

static void test_kernel_space(uint64_t kcr3) {
    uint64_t flags = 0, paddr = 0;
    CHECK(vmm_query(kcr3, 0x100000, &paddr, &flags) == 0);
    CHECK(paddr == 0x100000);
    CHECK(flags == (PAGE_PRESENT | PAGE_WRITE));              /* supervisor-only */
    CHECK(vmm_query(kcr3, 128ULL * 1024 * 1024, 0, 0) != 0);  /* just past the identity map */
    CHECK(vmm_query(kcr3, UVA, 0, 0) != 0);
}

static void test_basic_user_mapping(uint64_t kcr3) {
    long base = live_frames;
    uint64_t cr3 = vmm_create_address_space();
    CHECK(cr3 != 0 && cr3 != kcr3);

    uint64_t flags = 0, paddr = 0;
    CHECK(vmm_map_user_alloc(cr3, UVA, 3 * PAGE_SIZE, PAGE_USER | PAGE_WRITE) == 0);
    for (int i = 0; i < 3; i++) {
        CHECK(vmm_query(cr3, UVA + i * PAGE_SIZE, &paddr, &flags) == 0);
        CHECK(flags == P_W_U);
        /* frames are zeroed even though the fake PMM hands out 0xAA garbage */
        uint8_t* frame = (uint8_t*)(paddr & ~0xFFFULL);
        int zero = 1;
        for (int b = 0; b < 4096; b++) zero &= (frame[b] == 0);
        CHECK(zero);
    }
    CHECK(vmm_query(cr3, UVA + 3 * PAGE_SIZE, 0, 0) != 0);    /* one past the end */
    CHECK(vmm_query(cr3, UVA + 0x123, &paddr, 0) == 0 && (paddr & 0xFFF) == 0x123);

    /* Kernel half is shared but stays supervisor-only in the new space */
    CHECK(vmm_query(cr3, 0x100000, &paddr, &flags) == 0);
    CHECK(paddr == 0x100000 && flags == (PAGE_PRESENT | PAGE_WRITE));

    /* The user page does not exist in the kernel address space */
    CHECK(vmm_query(kcr3, UVA, 0, 0) != 0);

    vmm_destroy_address_space(cr3);
    CHECK(live_frames == base);
}

/* x86 checks U/S at EVERY paging level: a user leaf under a supervisor-only
 * table is still a ring-3 fault. */
static void test_user_bit_on_every_level(void) {
    long base = live_frames;
    uint64_t cr3 = vmm_create_address_space();
    uint64_t flags = 0;

    /* A single fresh mapping, nothing later to repair the tables */
    CHECK(vmm_map_user_alloc(cr3, UVA, PAGE_SIZE, PAGE_USER | PAGE_WRITE) == 0);
    CHECK(vmm_query(cr3, UVA, 0, &flags) == 0 && flags == P_W_U);

    /* Forge the old bug (existing table without U) and map next to it: the
     * mapping code must repair the levels instead of leaving them supervisor-only. */
    uint64_t* pml4 = (uint64_t*)cr3;
    uint64_t slot = (UVA >> 39) & 0x1FF;
    uint64_t* pdp = (uint64_t*)(pml4[slot] & 0x000FFFFFFFFFF000ULL);
    uint64_t pdp_slot = (UVA >> 30) & 0x1FF;
    pml4[slot] &= ~PAGE_USER;
    pdp[pdp_slot] &= ~PAGE_USER;
    CHECK(vmm_query(cr3, UVA, 0, &flags) == 0 && !(flags & PAGE_USER));
    CHECK(vmm_map_user_alloc(cr3, UVA + PAGE_SIZE, PAGE_SIZE, PAGE_USER | PAGE_WRITE) == 0);
    CHECK(vmm_query(cr3, UVA, 0, &flags) == 0 && flags == P_W_U);
    CHECK(vmm_query(cr3, UVA + PAGE_SIZE, 0, &flags) == 0 && flags == P_W_U);

    vmm_destroy_address_space(cr3);
    CHECK(live_frames == base);
}

static void test_rules(uint64_t kcr3) {
    long base = live_frames;
    uint64_t cr3 = vmm_create_address_space();
    uint64_t frame = (uint64_t)pmm_alloc_block();

    CHECK(vmm_map_page_in(cr3, UVA, frame, PAGE_USER | PAGE_WRITE | PAGE_NO_FREE) == 0);
    CHECK(vmm_map_page_in(cr3, UVA, frame, PAGE_USER | PAGE_WRITE) != 0);          /* already mapped */
    CHECK(vmm_map_page_in(cr3, UVA + 0x10, frame, PAGE_USER) != 0);                /* unaligned vaddr */
    CHECK(vmm_map_page_in(cr3, UVA + PAGE_SIZE, frame + 8, PAGE_USER) != 0);       /* unaligned paddr */
    CHECK(vmm_map_page_in(cr3, UVA + PAGE_SIZE, frame, PAGE_WRITE) != 0);          /* user range, no U */
    CHECK(vmm_map_page_in(cr3, 0x300000, frame, PAGE_USER | PAGE_WRITE) != 0);     /* kernel range with U */
    CHECK(vmm_map_page_in(cr3, 0x300000, frame, PAGE_WRITE) != 0);                 /* kernel range in process tables */
    CHECK(vmm_map_page_in(kcr3, UVA + PAGE_SIZE, frame, PAGE_USER) != 0);          /* user range in kernel tables */
    CHECK(vmm_map_page_in(cr3, USER_SPACE_END, frame, PAGE_USER) != 0);            /* non-canonical */
    CHECK(vmm_map_page_in(0, UVA, frame, PAGE_USER) != 0);

    CHECK(vmm_map_user_alloc(cr3, UVA + PAGE_SIZE, 0, PAGE_USER) != 0);            /* size 0 */
    CHECK(vmm_map_user_alloc(cr3, USER_SPACE_END - PAGE_SIZE, 2 * PAGE_SIZE, PAGE_USER) != 0);
    CHECK(vmm_map_user_alloc(cr3, UVA, (size_t)-1, PAGE_USER) != 0);               /* overflow */
    CHECK(vmm_map_user_alloc(cr3, USER_SPACE_END - PAGE_SIZE, PAGE_SIZE, PAGE_USER | PAGE_WRITE) == 0);

    /* stack region in the last user PML4 slot */
    CHECK(vmm_map_user_alloc(cr3, USER_STACK_TOP - 8 * PAGE_SIZE, 8 * PAGE_SIZE, PAGE_USER | PAGE_WRITE) == 0);
    uint64_t flags;
    CHECK(vmm_query(cr3, USER_STACK_TOP - PAGE_SIZE, 0, &flags) == 0 && flags == P_W_U);

    /* PAGE_NO_FREE frames survive destroy; the rest are returned */
    vmm_destroy_address_space(cr3);
    CHECK(live_frames == base + 1);
    pmm_free_block((void*)frame);
    CHECK(live_frames == base);
}

static void test_spanning_tables(void) {
    long base = live_frames;
    uint64_t cr3 = vmm_create_address_space();

    /* 1200 pages crosses two page-table boundaries, starting mid-table */
    uint64_t start = UVA + 100 * PAGE_SIZE;
    CHECK(vmm_map_user_alloc(cr3, start, 1200 * PAGE_SIZE, PAGE_USER | PAGE_WRITE) == 0);
    int all_ok = 1;
    for (int i = 0; i < 1200; i++) {
        uint64_t flags = 0;
        all_ok &= (vmm_query(cr3, start + (uint64_t)i * PAGE_SIZE, 0, &flags) == 0 && flags == P_W_U);
    }
    CHECK(all_ok);

    /* unmap one page: gone, frame returned, double unmap rejected */
    long before = live_frames;
    CHECK(vmm_unmap_page_in(cr3, start + 5 * PAGE_SIZE, 1) == 0);
    CHECK(live_frames == before - 1);
    CHECK(vmm_query(cr3, start + 5 * PAGE_SIZE, 0, 0) != 0);
    CHECK(vmm_unmap_page_in(cr3, start + 5 * PAGE_SIZE, 1) != 0);
    CHECK(vmm_query(cr3, start + 6 * PAGE_SIZE, 0, 0) == 0);

    vmm_destroy_address_space(cr3);
    CHECK(live_frames == base);
}

static void test_clone_address_space(void) {
    long base = live_frames;
    uint64_t source = vmm_create_address_space();
    CHECK(source != 0);
    CHECK(vmm_map_user_alloc(source, UVA, 2 * PAGE_SIZE,
                             PAGE_USER | PAGE_WRITE) == 0);
    uint64_t source_pa = 0, source_second_pa = 0;
    CHECK(vmm_query(source, UVA, &source_pa, 0) == 0);
    CHECK(vmm_query(source, UVA + PAGE_SIZE, &source_second_pa, 0) == 0);
    ((uint8_t*)(uintptr_t)source_pa)[17] = 0x5A;
    ((uint8_t*)(uintptr_t)source_second_pa)[31] = 0xA5;

    uint64_t clone = vmm_clone_address_space(source);
    CHECK(clone != 0 && clone != source);
    if (clone) {
        uint64_t clone_pa = 0, second_pa = 0;
        CHECK(vmm_query(clone, UVA, &clone_pa, 0) == 0);
        CHECK(vmm_query(clone, UVA + PAGE_SIZE, &second_pa, 0) == 0);
        CHECK((clone_pa & ~0xFFFULL) != (source_pa & ~0xFFFULL));
        CHECK(((uint8_t*)(uintptr_t)clone_pa)[17] == 0x5A);
        CHECK(((uint8_t*)(uintptr_t)second_pa)[31] == 0xA5);
        ((uint8_t*)(uintptr_t)clone_pa)[17] = 0xCC;
        CHECK(((uint8_t*)(uintptr_t)source_pa)[17] == 0x5A);
        vmm_destroy_address_space(clone);
    }

    vmm_destroy_address_space(source);
    CHECK(live_frames == base);

    long baseline = live_frames;
    source = vmm_create_address_space();
    CHECK(source != 0);
    CHECK(vmm_map_user_alloc(source, UVA, PAGE_SIZE,
                             PAGE_USER | PAGE_WRITE) == 0);
    base = live_frames;
    budget = 1;
    CHECK(vmm_clone_address_space(source) == 0);
    budget = -1;
    CHECK(live_frames == base);
    vmm_destroy_address_space(source);
    CHECK(live_frames == baseline);
}

/* Fail the PMM after N allocations for many N: every attempt must either fully
 * succeed or leave nothing mapped, and destroy must never leak. */
static void test_failure_injection(void) {
    long base = live_frames;
    int successes = 0, failures_seen = 0;

    for (long n = 0; n <= 1600; n += (n < 40 ? 1 : 37)) {
        uint64_t cr3 = vmm_create_address_space();
        CHECK(cr3 != 0);

        uint64_t start = UVA + 500 * PAGE_SIZE;
        uint64_t pages = 1100;           /* spans more than two page tables */

        budget = n;
        int rc = vmm_map_user_alloc(cr3, start, pages * PAGE_SIZE, PAGE_USER | PAGE_WRITE);
        budget = -1;

        if (rc == 0) {
            successes++;
            CHECK(vmm_query(cr3, start, 0, 0) == 0);
            CHECK(vmm_query(cr3, start + (pages - 1) * PAGE_SIZE, 0, 0) == 0);
        } else {
            failures_seen++;
            int none = 1;
            for (uint64_t i = 0; i < pages; i++) {
                none &= (vmm_query(cr3, start + i * PAGE_SIZE, 0, 0) != 0);
            }
            CHECK(none);                 /* all-or-nothing */
        }

        vmm_destroy_address_space(cr3);
        CHECK(live_frames == base);
    }
    CHECK(successes > 0 && failures_seen > 0);

    /* address space creation itself failing */
    budget = 0;
    CHECK(vmm_create_address_space() == 0);
    budget = -1;
    CHECK(live_frames == base);
}

static void test_destroy_active(uint64_t kcr3) {
    long base = live_frames;
    uint64_t cr3 = vmm_create_address_space();
    CHECK(vmm_map_user_alloc(cr3, UVA, PAGE_SIZE, PAGE_USER | PAGE_WRITE) == 0);

    vmm_switch(cr3);
    CHECK(vmm_current_cr3() == cr3);
    vmm_destroy_address_space(cr3);
    CHECK(vmm_current_cr3() == kcr3);           /* never left standing on freed tables */
    CHECK(live_frames == base);

    vmm_destroy_address_space(kcr3);            /* destroying the kernel space is refused */
    vmm_destroy_address_space(0);
    CHECK(vmm_query(kcr3, 0x100000, 0, 0) == 0);
}

int main(void) {
    vmm_init();
    uint64_t kcr3 = vmm_kernel_cr3();
    CHECK(kcr3 != 0);
    CHECK(vmm_current_cr3() == kcr3);

    test_kernel_space(kcr3);
    test_basic_user_mapping(kcr3);
    test_user_bit_on_every_level();
    test_rules(kcr3);
    test_spanning_tables();
    test_clone_address_space();
    test_failure_injection();
    test_destroy_active(kcr3);

    printf("vmm_test: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
