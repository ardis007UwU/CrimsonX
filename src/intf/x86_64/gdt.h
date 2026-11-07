#pragma once

#include <stdint.h>

#define GDT_RING_0 0
#define GDT_RING_1 1 // legacy
#define GDT_RING_2 2 // legacy
#define GDT_RING_3 3

#define GDT_SELECTOR_INDEX 3

#define GDT_SELECTOR_CS_KERNEL ((1 << GDT_SELECTOR_INDEX) | GDT_RING_0)

/*
 * Segment layout (the order is required by SYSRET, which derives the user
 * selectors from STAR[63:48]: SS = base + 8, CS = base + 16):
 *
 *   0x00  null
 *   0x08  kernel code   (ring 0)
 *   0x10  kernel data   (ring 0)   <- STAR[63:48] base for SYSRET
 *   0x18  user data     (ring 3)   -> 0x1B with RPL 3
 *   0x20  user code     (ring 3)   -> 0x23 with RPL 3
 *   0x28  TSS (16 bytes, occupies two slots)
 */
#define GDT_SEL_KERNEL_CODE 0x08
#define GDT_SEL_KERNEL_DATA 0x10
#define GDT_SEL_USER_DATA   (0x18 | GDT_RING_3)
#define GDT_SEL_USER_CODE   (0x20 | GDT_RING_3)
#define GDT_SEL_TSS         0x28

/* IST slot used by the double fault handler (1-based, as written in the IDT). */
#define GDT_IST_DOUBLE_FAULT 1

void gdt_init(void);

/* Stack the CPU switches to when an interrupt or exception arrives from ring 3.
 * Must be updated on every context switch to a user task. */
void gdt_set_kernel_stack(uint64_t rsp0);
