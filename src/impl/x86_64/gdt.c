#include <stdint.h>
#include "x86_64/gdt.h"

#define DF_STACK_SIZE 8192

struct Tss {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed));

struct Gdt {
    uint64_t entries[5];   /* null, kcode, kdata, udata, ucode */
    uint64_t tss_low;      /* 64-bit TSS descriptor is 16 bytes */
    uint64_t tss_high;
} __attribute__((packed));

struct GdtPtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

_Static_assert(sizeof(struct Tss) == 104, "TSS must be 104 bytes");
_Static_assert(sizeof(struct Gdt) == 56, "GDT must be 5 entries + 16 byte TSS descriptor");

extern void gdt_flush(struct GdtPtr* gdt_ptr);

static struct Gdt gdt __attribute__((aligned(16)));
static struct GdtPtr gdt_ptr;
static struct Tss tss __attribute__((aligned(16)));

/* Separate stack for #DF so a blown kernel stack shows up as a message
 * instead of a triple fault. */
static uint8_t df_stack[DF_STACK_SIZE] __attribute__((aligned(16)));

void gdt_set_kernel_stack(uint64_t rsp0) {
    tss.rsp0 = rsp0;
}

void gdt_init(void) {
    /*
     * Classic flat descriptors. In long mode base/limit are ignored for
     * code and data; what matters is access byte (P, DPL, S, type) and the
     * L bit (bit 53) on code segments.
     *
     *   access 0x9A = P | DPL0 | S | code | readable
     *   access 0x92 = P | DPL0 | S | data | writable
     *   access 0xFA = P | DPL3 | S | code | readable
     *   access 0xF2 = P | DPL3 | S | data | writable
     *   flags  0xA  = G | L      (code)      0xC = G | D/B (data)
     */
    gdt.entries[0] = 0x0000000000000000ULL;   /* null        */
    gdt.entries[1] = 0x00AF9A000000FFFFULL;   /* 0x08 kcode  */
    gdt.entries[2] = 0x00CF92000000FFFFULL;   /* 0x10 kdata  */
    gdt.entries[3] = 0x00CFF2000000FFFFULL;   /* 0x18 udata  */
    gdt.entries[4] = 0x00AFFA000000FFFFULL;   /* 0x20 ucode  */

    /* TSS: no I/O permission bitmap (offset == size) so ring 3 gets no port access. */
    for (unsigned i = 0; i < sizeof(tss); i++) {
        ((uint8_t*)&tss)[i] = 0;
    }
    tss.iopb_offset = sizeof(struct Tss);
    tss.ist[GDT_IST_DOUBLE_FAULT - 1] = (uint64_t)df_stack + DF_STACK_SIZE;

    uint64_t base  = (uint64_t)&tss;
    uint64_t limit = sizeof(struct Tss) - 1;

    gdt.tss_low = (limit & 0xFFFFULL)
                | ((base & 0xFFFFFFULL) << 16)
                | (0x89ULL << 40)                       /* P | available 64-bit TSS */
                | (((limit >> 16) & 0xFULL) << 48)
                | (((base >> 24) & 0xFFULL) << 56);
    gdt.tss_high = base >> 32;

    gdt_ptr.limit = sizeof(struct Gdt) - 1;
    gdt_ptr.base  = (uint64_t)&gdt;

    gdt_flush(&gdt_ptr);

    uint16_t tss_sel = GDT_SEL_TSS;
    asm volatile("ltr %0" : : "r"(tss_sel));
}
