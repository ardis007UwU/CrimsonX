extern exception_handler

global isr_stub_table

section .text
bits 64

; Stub for vectors where the CPU does NOT push an error code:
; push a dummy 0 so every frame has the same shape.
%macro ISR_NOERR 1
isr_stub_%1:
    push qword 0
    push qword %1
    jmp isr_common
%endmacro

; Stub for vectors where the CPU already pushed an error code.
%macro ISR_ERR 1
isr_stub_%1:
    push qword %1
    jmp isr_common
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

; Common path. Stack on entry (top first): vector, error code, then the
; CPU frame (rip, cs, rflags, rsp, ss). Matches struct exception_frame.
; Alignment: CPU aligns RSP to 16 before pushing the frame; frame (5 qwords)
; + error code + vector + 15 saved registers = 22 qwords = 176 bytes, so RSP
; is 16-byte aligned again at the call below.
isr_common:
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
    mov rdi, rsp            ; arg0 = struct exception_frame*
    call exception_handler

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
    add rsp, 16             ; drop vector + error code
    iretq

section .rodata
align 8
isr_stub_table:
%assign i 0
%rep 32
    dq isr_stub_%+ i
%assign i i+1
%endrep
