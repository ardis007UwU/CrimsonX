OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

/* Everything CrimsonX user binaries need: image mapped above USER_SPACE_START
 * (the kernel's user half starts at PML4 slot 1 == 512 GiB). */
SECTIONS
{
    . = 0x8000000000;

    .text : {
        *(.text)
    }

    .data : {
        *(.data)
    }

    /DISCARD/ : {
        *(.comment)
        *(.eh_frame)
    }
}
