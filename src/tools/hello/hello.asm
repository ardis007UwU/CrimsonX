; hello.asm — CrimsonX first userland citizen
;
; Runs in ring 3 at 0x8000000000 (PML4 slot 1: any USER_SPACE_START page).
; Syscall ABI (syscall.h):
;   0 WRITE_LEGACY  rdi = buf, rsi = len
;   2 EXIT   rdi = status
;   3 READ   rdi = fd, rsi = buf, rdx = count   (POSIX style)

BITS 64
global _start

section .text
_start:
    ; SYS_WRITE_LEGACY "hello\n"
    lea rdi, [rel msg]
    mov rsi, 6
    xor rax, rax            ; SYS_WRITE_LEGACY
    syscall

    ; SYS_READ 4 bytes from stdin (blocks in the kernel)
    mov rax, 3              ; SYS_READ
    mov rdi, 0              ; fd = stdin
    lea rsi, [rel rbuf]
    mov rdx, 4
    syscall
    cmp rax, 0
    jle .exit_ok

    ; echo back exactly what arrived
    lea rdi, [rel rbuf]
    mov rsi, rax
    xor rax, rax            ; SYS_WRITE_LEGACY
    syscall

.exit_ok:
    mov rdi, 0
    mov rax, 2              ; SYS_EXIT
    syscall

section .data
msg:  db "hello", 10
rbuf: dq 0
