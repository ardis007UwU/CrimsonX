; 64-bit syscall entry stub.
;
; Frame layout matches struct syscall_frame in syscall.h (fields ascend
; r15..rax, rip, flags, rsp; the pushes below descend the stack, so the
; first push lands at the highest address).
;
; All syscalls snapshot onto the task's KERNEL stack (tss-style jump through
; current_task->kernel_stack_top) instead of the caller's user stack — this
; keeps execve/exit teardown safe even when the calling address space dies.
;
; Globals used instead of registers as scratch, because every GP register is
; part of the saved frame (rax also carries the syscall number).

extern syscall_handler
extern current_task
extern syscall_scratch_rsp
extern syscall_scratch_rax
extern vmm_destroy_address_space
extern vmm_switch

; task_t.kernel_stack_top offset (process.c static-asserts == 56)
%define TASK_KSTACK_TOP 56

section .text
bits 64

global syscall_entry
global exec_drop
global fork_child_start

syscall_entry:
    ; IF is already cleared (SFMASK). Park user RSP and RAX in kernel memory
    ; first, since every GP register must land in the frame unharmed.
    mov [rel syscall_scratch_rsp], rsp
    mov [rel syscall_scratch_rax], rax

    mov rax, [rel current_task]
    mov rsp, [rax + TASK_KSTACK_TOP]        ; switch to the task kernel stack

    push qword [rel syscall_scratch_rsp]    ; user_rsp
    push r11                                ; user_rflags
    push rcx                                ; user_rip

    mov rax, [rel syscall_scratch_rax]
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    cld
    mov rdi, rsp                            ; arg0 = struct syscall_frame*
    call syscall_handler

syscall_return:
    ; RAX contains the return value from syscall_handler
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    add rsp, 8                  ; skip saved RAX to keep the handler's rax

    pop rcx                     ; return RIP -> RCX for sysret
    pop r11                     ; return RFLAGS -> R11 for sysret
    pop rsp                     ; restore user RSP

    o64 sysret

; A fork child starts with RDI pointing at its copied syscall frame.
fork_child_start:
    mov ax, 0x1B
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov rsp, rdi
    xor eax, eax
    jmp syscall_return

; void exec_drop(uint64_t old_cr3, uint64_t new_cr3,
;                void* entry, void* ustack, void* kstack_top)
;   RDI = old cr3  (0 skips the destroy: the spawn path starts on the shared
;                  kernel space, which must never be torn down)
;   RSI = new cr3
;   RDX = user entry point
;   RCX = user stack top
;   R8  = kernel stack top (TSS rsp0 for this task)
;
; Called from the syscall handler with IF off. After switching to the
; kernel stack, the old address space is freed (now safe: we no longer stand
; on its stack), the new CR3 is loaded via vmm_switch so the VMM's
; current-cr3 state stays coherent, and execution drops into ring 3.
; Never returns.
exec_drop:
    mov rsp, r8                 ; abandon the old (possibly user) stack
    test rdi, rdi
    jz .no_destroy

    push rsi
    push rdx
    push rcx
    call vmm_destroy_address_space      ; RDI = old cr3; may switch CR3 itself
    pop rcx
    pop rdx
    pop rsi

.no_destroy:
    push rdx
    push rcx
    mov rdi, rsi
    call vmm_switch                     ; keeps vmm's current_cr3 coherent
    pop rcx
    pop rdx

    mov ax, 0x1B
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    push qword 0x1B                     ; user SS
    push rcx                            ; user RSP
    push qword 0x202                    ; RFLAGS (IF enabled)
    push qword 0x23                     ; user CS
    push rdx                            ; user RIP
    iretq
