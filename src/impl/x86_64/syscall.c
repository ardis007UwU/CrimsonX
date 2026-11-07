#include "x86_64/syscall.h"
#include "x86_64/msr.h"
#include "x86_64/gdt.h"
#include "print.h"
#include "x86_64/pit.h"
#include "kernel/process.h"

extern void syscall_entry(void);

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

uint64_t syscall_handler(struct syscall_frame* frame) {
    uint64_t sys_num = frame->rax;

    switch (sys_num) {
        case SYS_WRITE:
            /* RDI = char* buf, RSI = length */
            print_str((char*)frame->rdi);
            return 0;

        case SYS_SLEEP:
            /* RDI = milliseconds */
            process_sleep((uint32_t)frame->rdi);
            return 0;

        case SYS_EXIT:
            /* RDI = exit code */
            print_str("\n[User Process Exited]\n");
            while (1) { asm volatile("hlt"); }
            return 0;

        default:
            print_str("\n[Kernel] Unknown Syscall!\n");
            return (uint64_t)-1;
    }
}