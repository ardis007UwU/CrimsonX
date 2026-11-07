global gdt_flush

section .text
bits 64

; void gdt_flush(struct GdtPtr* gdt_ptr)   RDI = pointer to {limit, base}
gdt_flush:
    lgdt [rdi]

    ; reload data segments with the kernel data selector (0x10)
    ; (FS/GS are left alone so a future GS base is not clobbered)
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; reload CS with the kernel code selector (0x08) via a far return
    pop rax            ; return address
    push qword 0x08
    push rax
    retfq
