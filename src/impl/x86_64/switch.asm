global switch_to
global get_rsp

section .text
bits 64

get_rsp:
    mov rax, rsp
    ret

switch_to:
    ; RDI = next task pointer, RSI = current task pointer
    
    ; 1. Save current task CPU registers onto current stack
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

    ; 2. Store current stack pointer in current_task->rsp (offset 8)
    mov [rsi + 8], rsp

    ; 3. Load next task stack pointer from next_task->rsp (offset 8)
    mov rsp, [rdi + 8]

    ; 4. Restore next task CPU registers
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
    pop rax

    ret