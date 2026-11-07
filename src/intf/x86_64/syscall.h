#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include <stddef.h>

/*
 * CrimsonX syscall ABI
 * --------------------
 * rax   number
 * rdi, rsi, rdx, r10, r8, r9   arguments
 *
 *  0 SYS_WRITE   legacy demo ABI: rdi = buf, rsi = len   -> stdout (fd 1)
 *  1 SYS_SLEEP   rdi = milliseconds
 *  2 SYS_EXIT    rdi = exit status (terminates the caller's task)
 *  3 SYS_READ    rdi = fd (only 0/stdin), rsi = buf, rdx = count
 *  4 SYS_YIELD   (reschedules eagerly)
 *  5 SYS_EXECVE  rdi = path (NUL-terminated string in user memory)
 *
 * The entry stub saves this frame on the task's KERNEL stack:
 */
#define SYS_WRITE  0
#define SYS_SLEEP  1
#define SYS_EXIT   2
#define SYS_READ   3
#define SYS_YIELD  4
#define SYS_EXECVE 5

struct syscall_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t user_rip;    /* Saved from RCX by CPU */
    uint64_t user_rflags; /* Saved from R11 by CPU */
    uint64_t user_rsp;    /* Saved manually before stack switch */
};

void syscall_init(void);
uint64_t syscall_handler(struct syscall_frame* frame);

/* Reachable from syscall_entry.asm */
extern uint64_t syscall_scratch_rsp;
extern uint64_t syscall_scratch_rax;

#endif
