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
 *  0 SYS_WRITE_LEGACY: rdi = buf, rsi = len -> stdout (fd 1)
 *  1 SYS_SLEEP   rdi = milliseconds
 *  2 SYS_EXIT    rdi = exit status (terminates the caller's task)
 *  3 SYS_READ    rdi = fd, rsi = buf, rdx = count
 *  4 SYS_YIELD   (reschedules eagerly)
 *  5 SYS_EXECVE  rdi = path, rsi = argv[], rdx = envp[] (user pointers)
 *  6 SYS_OPEN    rdi = path, rsi = access mode (0=read, 1=write, 2=read/write)
 *  7 SYS_CLOSE   rdi = fd
 *  8 SYS_LSEEK   rdi = fd, rsi = signed offset, rdx = whence
 *  9 SYS_WRITE    rdi = fd, rsi = buf, rdx = count
 * 10 SYS_STAT    rdi = path, rsi = pointer to syscall_stat_t
 * 11 SYS_WAITPID rdi = pid, rsi = status pointer (optional), rdx = options (0)
 * 12 SYS_FORK    no arguments; child returns zero, parent returns child PID
 * 13 SYS_BRK     rdi = requested program break (0 queries current value)
 * 14 SYS_SBRK    rdi = signed increment; returns previous break
 * 15 SYS_MMAP    rdi=addr,rsi=len,rdx=prot,r10=flags,r8=fd,r9=offset
 * 16 SYS_PIPE    rdi = user int[2] receiving read/write descriptors
 * 17 SYS_MUNMAP  rdi = mapped address, rsi = mapped length
 *
 * mmap currently supports read/write permissions, private file mappings, and
 * anonymous private/shared mappings. A zero address selects a free range;
 * nonzero addresses are exact placements. munmap removes a complete mapping.
 *
 * The entry stub saves this frame on the task's KERNEL stack:
 */
#define SYS_WRITE_LEGACY 0
#define SYS_SLEEP  1
#define SYS_EXIT   2
#define SYS_READ   3
#define SYS_YIELD  4
#define SYS_EXECVE 5
#define SYS_OPEN   6
#define SYS_CLOSE  7
#define SYS_LSEEK  8
#define SYS_WRITE  9
#define SYS_STAT   10
#define SYS_WAITPID 11
#define SYS_FORK  12
#define SYS_BRK   13
#define SYS_SBRK  14
#define SYS_MMAP  15
#define SYS_PIPE  16
#define SYS_MUNMAP 17

#define SYS_PROT_READ  1
#define SYS_PROT_WRITE 2
#define SYS_PROT_EXEC  4
#define SYS_MAP_SHARED  1
#define SYS_MAP_PRIVATE 2
#define SYS_MAP_ANONYMOUS 4

/** Read-only open mode accepted in the SYS_OPEN rsi argument. */
#define SYS_OPEN_RDONLY 0
/** Write-only open mode accepted in the SYS_OPEN rsi argument. */
#define SYS_OPEN_WRONLY 1
/** Read/write open mode accepted in the SYS_OPEN rsi argument. */
#define SYS_OPEN_RDWR   2

/** Minimal file metadata returned by SYS_STAT. */
typedef struct {
    uint32_t st_mode; /**< POSIX-compatible file type and permission bits. */
    uint32_t st_size; /**< File size in bytes. */
} syscall_stat_t;

#define SYS_S_IFMT  0170000U
#define SYS_S_IFDIR 0040000U
#define SYS_S_IFREG 0100000U
#define SYS_WEXITSTATUS(status) (((status) >> 8) & 0xFF)

struct syscall_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t user_rip;    /* Saved from RCX by CPU */
    uint64_t user_rflags; /* Saved from R11 by CPU */
    uint64_t user_rsp;    /* Saved manually before stack switch */
};

/** Configure the x86-64 SYSCALL MSRs and entry point. */
void syscall_init(void);
/** Dispatch a syscall using the register frame built by syscall_entry.asm. */
uint64_t syscall_handler(struct syscall_frame* frame);

/** Scratch storage used by syscall_entry.asm while switching stacks. */
extern uint64_t syscall_scratch_rsp;
/** Scratch storage used by syscall_entry.asm to save the syscall result. */
extern uint64_t syscall_scratch_rax;

#endif
