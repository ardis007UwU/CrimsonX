#include "x86_64/exceptions.h"
#include "print.h"
#include "kernel/process.h"

static char* exception_names[32] = {
    "#DE Divide Error",
    "#DB Debug",
    "NMI",
    "#BP Breakpoint",
    "#OF Overflow",
    "#BR Bound Range Exceeded",
    "#UD Invalid Opcode",
    "#NM Device Not Available",
    "#DF Double Fault",
    "Coprocessor Segment Overrun",
    "#TS Invalid TSS",
    "#NP Segment Not Present",
    "#SS Stack-Segment Fault",
    "#GP General Protection Fault",
    "#PF Page Fault",
    "Reserved",
    "#MF x87 Floating-Point",
    "#AC Alignment Check",
    "#MC Machine Check",
    "#XM SIMD Floating-Point",
    "#VE Virtualization",
    "#CP Control Protection",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "#HV Hypervisor Injection",
    "#VC VMM Communication",
    "#SX Security",
    "Reserved"
};

static void print_field(char* label, uint64_t value) {
    print_str(label);
    print_str("0x");
    print_uint64_hex(value);
}

void exception_handler(struct exception_frame* frame) {
    uint64_t cr2 = 0;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));

    if (frame && frame->vector == 14 && (frame->cs & 3) == 3) {
        task_t* task = process_current_task();
        print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_RED);
        print_str("\n[user fault] page fault in PID ");
        print_uint64_dec(task ? task->id : 0);
        print_str(" at 0x");
        print_uint64_hex(cr2);
        print_str("; terminating with SIGSEGV\n");
        print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK);
        if (task) {
            process_send_signal((int64_t)task->id, KERNEL_SIGSEGV);
            process_handle_pending_signal();
        }
        for (;;) asm volatile("cli; hlt");
    }

    print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_RED);
    print_str("\n*** KERNEL EXCEPTION: ");
    print_str(frame->vector < 32 ? exception_names[frame->vector] : "Unknown");
    print_str(" ***\n");
    print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK);

    print_field("vector=", frame->vector);
    print_field("  err=", frame->error_code);
    print_str("\n");
    print_field("RIP=", frame->rip);
    print_field("  CS=", frame->cs);
    print_field("  RFLAGS=", frame->rflags);
    print_str("\n");
    print_field("RSP=", frame->rsp);
    print_field("  SS=", frame->ss);
    print_str(frame->cs & 3 ? "  (from USER mode)\n" : "  (from KERNEL mode)\n");

    if (frame->vector == 14) {
        /* #PF: CR2 holds the faulting address, error code says why */
        print_field("CR2=", cr2);
        print_str(frame->error_code & 1  ? "  protection-violation" : "  page-not-present");
        print_str(frame->error_code & 2  ? "  write" : "  read");
        print_str(frame->error_code & 4  ? "  user" : "  supervisor");
        if (frame->error_code & 8)  print_str("  reserved-bit-set");
        if (frame->error_code & 16) print_str("  instruction-fetch");
        print_str("\n");
    } else if (frame->vector == 13 && frame->error_code != 0) {
        /* #GP with a selector error code: bit0 = external, bits 3.. = index */
        print_field("selector index=", frame->error_code >> 3);
        print_str("\n");
    }

    print_field("RAX=", frame->rax);
    print_field("  RBX=", frame->rbx);
    print_field("  RCX=", frame->rcx);
    print_str("\n");
    print_field("RDX=", frame->rdx);
    print_field("  RSI=", frame->rsi);
    print_field("  RDI=", frame->rdi);
    print_str("\n");

    print_str("System halted.\n");

    /* Nothing sane to return to yet. Later this can kill the offending
     * user task when the fault came from ring 3. */
    while (1) {
        asm volatile("cli; hlt");
    }
}
