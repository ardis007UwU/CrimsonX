#include "x86_64/syscall.h"
#include "x86_64/msr.h"
#include "x86_64/gdt.h"
#include "x86_64/pit.h"
#include "keyboard.h"
#include "print.h"
#include "kernel/process.h"
#include "kernel/irq.h"
#include "fs/vfs.h"
#include "kernel/exec.h"
#include "mm/vmm.h"

extern void syscall_entry(void);

uint64_t syscall_scratch_rsp = 0;
uint64_t syscall_scratch_rax = 0;

void syscall_init(void) {
    /* 1. Enable System Call Extensions in EFER */
    uint64_t efer = rdmsr(IA32_EFER);
    wrmsr(IA32_EFER, efer | EFER_SCE);

    /* 2. Configure STAR MSR:
     * Bits 47:32 = SYSRET base selector (0x10: User SS=0x18|3=0x1B, User CS=0x20|3=0x23)
     * Bits 31:16 = SYSCALL base selector (0x08: Kernel CS=0x08, Kernel SS=0x10)
     */
    uint64_t star = ((uint64_t)GDT_SEL_KERNEL_DATA << 48) | ((uint64_t)GDT_SEL_KERNEL_CODE << 32);
    wrmsr(MSR_STAR, star);

    /* 3. Set LSTAR MSR to syscall entry address */
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);

    /* 4. Mask RFLAGS.IF (0x200) on entry so interrupts are off during stack swap */
    wrmsr(MSR_SFMASK, 0x200);
}

/* Copy a NUL-terminated string from the calling task's address space into a
 * kernel buffer, validating every page with the VMM walker. Addresses must
 * live in user space (kernel callers never take this path for execve). */
static int read_user_string(char* dst, const void* usrc, size_t max) {
    uint64_t addr = (uint64_t)usrc;
    uint64_t cr3 = vmm_current_cr3();
    if (addr < USER_SPACE_START) return -1;   /* only user memory, never the kernel half */
    size_t i = 0;

    while (i < max - 1) {
        uint64_t va = addr + i;
        if (va >= USER_SPACE_END) return -1;
        uint64_t page = va & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t paddr;
        if (vmm_query(cr3, page, &paddr, 0) != 0) return -1;   /* unmapped: no access */
        char c = *(const volatile char*)(size_t)(paddr + (size_t)(va - page));
        dst[i++] = c;
        if (c == '\0') break;
    }
    dst[i] = '\0';
    return 0;
}

uint64_t syscall_handler(struct syscall_frame* frame) {
    task_t* task = process_current_task();
    uint64_t sys_num = frame->rax;

    switch (sys_num) {
        case SYS_WRITE: {
            /* Legacy demo ABI: RDI = char* buf, RSI = length (goes to fd 1) */
            vfs_file_t* out = task ? task->fds[VFS_FD_STDOUT] : NULL;
            size_t len = frame->rsi;
            if (len > 0x100000) len = 0x100000;

            if (out && out->node) {
                int n = vfs_file_write(out, (const void*)frame->rdi, len);
                return n < 0 ? (uint64_t)-1 : (uint64_t)n;
            }
            /* No console deskriptor (boot context fallback): raw print. */
            print_str((char*)frame->rdi);
            return (uint64_t)len;
        }

        case SYS_SLEEP:
            /* RDI = milliseconds */
            process_sleep((uint32_t)frame->rdi);
            return 0;

        case SYS_EXIT:
            /* RDI = exit code (unused today; task teardown runs the rest) */
            (void)frame->rdi;
            process_kill_current();   /* never returns */

            /* uncroak: kill_current only reaches here with no other tasks */
            while (1) { asm volatile("cli; hlt"); }

        case SYS_READ: {
            if (frame->rdi != VFS_FD_STDIN) return (uint64_t)-1;
            size_t count = frame->rdx > 0x20000 ? 0x20000 : frame->rdx;
            char* ubuf = (char*)frame->rsi;
            if (!count) return 0;

            vfs_file_t* in = task ? task->fds[VFS_FD_STDIN] : NULL;
            if (in && in->node) {
                int n = vfs_file_read(in, ubuf, count);
                if (n > 0) return (uint64_t)n;
                if (n < 0) return (uint64_t)-1;
            }
            /* Empty stdin: park the task until the keyboard IRQ queues one */
            int n = keyboard_read_blocking(ubuf, count);
            return n <= 0 ? (uint64_t)-1 : (uint64_t)n;
        }

        case SYS_YIELD:
            schedule();
            return 0;

        case SYS_EXECVE: {
            char kpath[320];
            if (!task || !task->kernel_stack_top) return (uint64_t)-1;
            if (read_user_string(kpath, (const void*)frame->rdi, sizeof(kpath)) != 0) {
                return (uint64_t)-1;
            }
            /* On success this drops into ring 3 and never returns. */
            if (exec_replace_current(task, kpath) != 0) {
                return (uint64_t)-1;
            }
            /* unreachable */
            return (uint64_t)-1;
        }

        default:
            print_str("\n[Kernel] Unknown Syscall!\n");
            return (uint64_t)-1;
    }
}
