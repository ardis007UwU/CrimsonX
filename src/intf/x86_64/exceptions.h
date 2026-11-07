#pragma once

#include <stdint.h>

/*
 * Register state saved by isr_common (exceptions_.asm). Field order is the
 * reverse of the push order, so r15 sits at the lowest address (RSP).
 * vector/error_code are pushed by the per-vector stubs; rip..ss are pushed
 * by the CPU.
 */
struct exception_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;   /* 0 for vectors where the CPU pushes none */
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

/* Entry addresses of the 32 CPU exception stubs, indexed by vector. */
extern uint64_t isr_stub_table[32];

/* Called from isr_common. Prints the fault and halts. */
void exception_handler(struct exception_frame* frame);
