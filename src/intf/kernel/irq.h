#ifndef KERNEL_IRQ_H
#define KERNEL_IRQ_H

#include <stdint.h>

#define RFLAGS_IF (1ull << 9)

typedef uint64_t irq_flags_t;

/** Disable interrupts and return the prior flags for irq_restore(). */
static inline irq_flags_t irq_save(void) {
    irq_flags_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

/** Restore the interrupt-enable state captured by irq_save(). */
static inline void irq_restore(irq_flags_t flags) {
    if (flags & RFLAGS_IF) asm volatile("sti" : : : "memory");
}

#endif
