#include "print.h"
#include "keyboard.h"
#include "shell.h"
#include "x86_64/port.h"
#include "x86_64/gdt.h"
#include "x86_64/idt.h"
#include "x86_64/pic.h"
#include "x86_64/pit.h"
#include "x86_64/syscall.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/kmalloc.h"
#include "kernel/process.h"
#include "blkdev.h"
#include "x86_64/ata.h"
#include "fs/vfs.h"
#include "fs/crimsonfs.h"

static void ata_storage_init(void) {
    print_set_color(PRINT_COLOR_CYAN, PRINT_COLOR_BLACK);
    print_str("[ata] probing IDE channels\n");

    ata_init();
    if (blkdev_count() == 0) {
        print_set_color(PRINT_COLOR_BROWN, PRINT_COLOR_BLACK);
        print_str("[ata] no IDE drive found\n");
    }
}

static void crimsonfs_boot_mount(void) {
    vfs_init();
    crimsonfs_init();

    if (blkdev_count() == 0) return;

    vfs_node_t* root = crimsonfs_mount(blkdev_get(0));
    if (root) {
        print_set_color(PRINT_COLOR_GREEN, PRINT_COLOR_BLACK);
        print_str("[crimsonfs] volume mounted at /\n");
    } else {
        print_set_color(PRINT_COLOR_BROWN, PRINT_COLOR_BLACK);
        print_str("[crimsonfs] no CrimsonFS volume on disk 0 (try: mkfs)\n");
    }
}

void task_worker(void) {
    uint64_t ticks = 0;
    while (1) {
        const char* label = "[T1 Active: ";
        uint8_t color = PRINT_COLOR_GREEN | (PRINT_COLOR_BLACK << 4);

        for (size_t i = 0; label[i] != '\0'; i++) {
            print_put_char_at(label[i], color, 60 + i, 0);
        }

        char count_digit = '0' + (ticks % 10);
        print_put_char_at(count_digit, color, 72, 0);
        print_put_char_at(']', color, 73, 0);

        ticks++;
        process_sleep(1000);
    }
}

void kernel_main(void) {
    /* Replace the minimal boot GDT with the full one (kernel/user segments + TSS)
     * before the IDT is built. Interrupts are still off here. */
    gdt_init();
    print_serial_init();

    print_clear();
    print_set_color(PRINT_COLOR_YELLOW, PRINT_COLOR_BLACK);
    print_str("Welcome to CrimsonX OS!\n\n");

    idt_init();
    pic_init();

    uint8_t pic_mask = port_inb(0x21);
    port_outb(0x21, pic_mask & ~0x01);

    pit_init(100);
    idt_set_handler_timer(pit_handler);

    keyboard_init();
    keyboard_set_handler(shell_handle_input);

    pmm_init(128ULL * 1024 * 1024);
    vmm_init();
    kmalloc_init();

    asm volatile("sti");

    process_init();
    process_create(task_worker);

    syscall_init();

    ata_storage_init();
    crimsonfs_boot_mount();

    shell_init();

    while (1) {
        shell_update();
        asm volatile("hlt");
    }
}