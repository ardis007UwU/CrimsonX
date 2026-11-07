; hello_data.asm — kernelside embed of the user binary (incbin'd at build
; time by the Makefile; built from src/tools/hello).

global hello_elf_start
global hello_elf_end

section .data
hello_elf_start:
    incbin "hello.elf"
hello_elf_end: