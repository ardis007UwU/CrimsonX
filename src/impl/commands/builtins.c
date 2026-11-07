#include "commands.h"
#include "print.h"
#include "string.h"
#include "x86_64/port.h"
#include "x86_64/pit.h"
#include "mm/pmm.h"
#include "mm/kmalloc.h"
#include "mm/vmm.h"
#include "kernel/process.h"
#include "kernel/exec.h"
#include "x86_64/syscall.h"
#include "fs/vfs.h"
#include "fs/crimsonfs.h"
#include "blkdev.h"

extern const uint8_t hello_elf_start[];
extern const uint8_t hello_elf_end[];

static int vmtest_failed;

static void vmtest_check(char* name, int ok) {
    print_set_color(ok ? PRINT_COLOR_GREEN : PRINT_COLOR_RED, PRINT_COLOR_BLACK);
    print_str(ok ? "  PASS  " : "  FAIL  ");
    print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK);
    print_str(name);
    print_str("\n");
    if (!ok) vmtest_failed = 1;
}

/* Exercises per-process address spaces on the real CPU. */
static void cmd_vmtest(void) {
    const uint64_t va = USER_SPACE_START + 0x1000;
    const uint64_t pattern_a = 0x1122334455667788ULL;
    const uint64_t pattern_b = 0xA5A5A5A5DEADBEEFULL;
    uint64_t paddr = 0, flags = 0;

    vmtest_failed = 0;
    print_set_color(PRINT_COLOR_CYAN, PRINT_COLOR_BLACK);
    print_str("vmtest: address-space checks\n");

    uint64_t free_before = pmm_get_free_block_count();
    uint64_t kernel_cr3 = vmm_kernel_cr3();

    uint64_t cr3 = vmm_create_address_space();
    vmtest_check("create address space", cr3 != 0 && cr3 != kernel_cr3);
    if (!cr3) return;

    vmtest_check("map user page (U|W)", vmm_map_user_alloc(cr3, va, PAGE_SIZE, PAGE_USER | PAGE_WRITE) == 0);

    int mapped = vmm_query(cr3, va, &paddr, &flags) == 0;
    vmtest_check("user page effective flags = P|W|U", mapped && flags == (PAGE_PRESENT | PAGE_WRITE | PAGE_USER));

    vmtest_check("user page invisible in kernel tables", vmm_query(kernel_cr3, va, 0, 0) != 0);

    int kmapped = vmm_query(cr3, 0x100000, 0, &flags) == 0;
    vmtest_check("kernel memory mapped in new space, supervisor-only",
                 kmapped && (flags & PAGE_USER) == 0);

    vmtest_check("kernel page rejected with U bit", vmm_map_page_in(cr3, 0x300000, 0x300000, PAGE_USER | PAGE_WRITE) != 0);
    vmtest_check("user page rejected without U bit", vmm_map_page_in(cr3, va + PAGE_SIZE, paddr & ~0xFFFULL, PAGE_WRITE) != 0);
    vmtest_check("user page rejected in kernel tables", vmm_map_page_in(kernel_cr3, va + PAGE_SIZE, paddr & ~0xFFFULL, PAGE_USER) != 0);

    if (mapped) {
        volatile uint64_t* frame = (volatile uint64_t*)(paddr & ~0xFFFULL);
        volatile uint64_t* user  = (volatile uint64_t*)va;
        uint64_t seen_in_user = 0, seen_in_kernel = 0;
        uint64_t saved_flags;

        asm volatile("pushfq; pop %0; cli" : "=r"(saved_flags) : : "memory");
        *frame = pattern_a;
        vmm_switch(cr3);
        seen_in_user = *user;
        *user = pattern_b;
        vmm_switch(kernel_cr3);
        seen_in_kernel = *frame;
        if (saved_flags & (1ULL << 9)) asm volatile("sti" : : : "memory");

        vmtest_check("read through user vaddr after CR3 switch", seen_in_user == pattern_a);
        vmtest_check("write via user vaddr lands in the frame", seen_in_kernel == pattern_b);
    }

    vmm_destroy_address_space(cr3);
    vmtest_check("destroy leaks no frames", pmm_get_free_block_count() == free_before);

    print_set_color(vmtest_failed ? PRINT_COLOR_RED : PRINT_COLOR_GREEN, PRINT_COLOR_BLACK);
    print_str(vmtest_failed ? "vmtest: FAILED\n" : "vmtest: all checks passed\n");
}

static void sys_reboot(void) {
    uint8_t status = 0x02;
    while (status & 0x02) {
        status = port_inb(0x64);
    }
    port_outb(0x64, 0xFE);
    asm volatile("cli; hlt");
}

/* ------------------------- filesystem commands ------------------------- */

static void cmd_mkfs(void) {
    blkdev_t* dev = blkdev_get(0);
    if (!dev) {
        print_set_color(PRINT_COLOR_RED, PRINT_COLOR_BLACK);
        print_str("mkfs: no block device (is a disk attached?)\n");
        return;
    }

    if (vfs_get_root()) crimsonfs_unmount();

    if (crimsonfs_format(dev) != 0) {
        print_set_color(PRINT_COLOR_RED, PRINT_COLOR_BLACK);
        print_str("mkfs: format failed\n");
        return;
    }

    if (crimsonfs_mount(dev)) {
        print_set_color(PRINT_COLOR_GREEN, PRINT_COLOR_BLACK);
        print_str("mkfs: CrimsonFS volume created and mounted\n");
    } else {
        print_set_color(PRINT_COLOR_RED, PRINT_COLOR_BLACK);
        print_str("mkfs: formatted but mount failed\n");
    }
}

static void cmd_ls(void) {
    if (!vfs_get_root()) {
        print_set_color(PRINT_COLOR_BROWN, PRINT_COLOR_BLACK);
        print_str("ls: no CrimsonFS volume mounted (try: mkfs)\n");
        return;
    }

    /* A fresh "/" handle starts the directory stream at offset 0. */
    vfs_node_t* dir = vfs_open("/");
    if (!dir) {
        print_set_color(PRINT_COLOR_RED, PRINT_COLOR_BLACK);
        print_str("ls: mount broken\n");
        return;
    }

    print_set_color(PRINT_COLOR_CYAN, PRINT_COLOR_BLACK);
    print_str("name                 type\n");
    print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK);

    uint8_t block[512];
    for (;;) {
        int got = dir->read(dir, block, sizeof(block));
        if (got <= 0) break;
        for (size_t off = 0; off + sizeof(crimsonfs_dirent_t) <= (size_t)got;
             off += sizeof(crimsonfs_dirent_t)) {
            crimsonfs_dirent_t* e = (crimsonfs_dirent_t*)(block + off);
            if (e->inode == 0) continue;

            print_str(e->name);
            for (size_t pad = strlen(e->name); pad < 20; pad++) print_char(' ');
            if (e->type == CRIMFS_TYPE_DIR) {
                print_str("<dir>\n");
            } else {
                print_str("file\n");
            }
        }
        if (got < (int)sizeof(block)) break;   /* end of directory */
    }

    vfs_close(dir);
}

static void cmd_cat(const char* name) {
    vfs_node_t* n = vfs_open(name);
    if (!n) {
        print_set_color(PRINT_COLOR_RED, PRINT_COLOR_BLACK);
        print_str("cat: no such file: ");
        print_str((char*)name);
        print_str("\n");
        return;
    }
    if (n->type != VFS_TYPE_FILE) {
        print_set_color(PRINT_COLOR_RED, PRINT_COLOR_BLACK);
        print_str("cat: not a regular file\n");
        vfs_close(n);
        return;
    }

    uint8_t buf[64];
    size_t total = 0;
    while (total < n->size) {
        int got = n->read(n, buf, sizeof(buf));
        if (got <= 0) break;
        for (int i = 0; i < got; i++) print_char(buf[i]);
        total += (size_t)got;
    }
    print_char('\n');
    vfs_close(n);
}

static void cmd_rm(const char* name) {
    if (!vfs_get_root()) {
        print_set_color(PRINT_COLOR_RED, PRINT_COLOR_BLACK);
        print_str("rm: no CrimsonFS volume mounted\n");
        return;
    }

    if (!name || name[0] == '\0') {
        print_set_color(PRINT_COLOR_RED, PRINT_COLOR_BLACK);
        print_str("Usage: rm <filename>\n");
        return;
    }

    if (crimsonfs_unlink(vfs_get_root(), name) == 0) {
        print_set_color(PRINT_COLOR_GREEN, PRINT_COLOR_BLACK);
        print_str("rm: removed ");
        print_str((char*)name);
        print_str("\n");
    } else {
        print_set_color(PRINT_COLOR_RED, PRINT_COLOR_BLACK);
        print_str("rm: failed to remove ");
        print_str((char*)name);
        print_str("\n");
    }
}

static void cmd_install(void) {
    vfs_node_t* root = vfs_get_root();
    if (!root) {
        print_set_color(PRINT_COLOR_RED, PRINT_COLOR_BLACK);
        print_str("install: no filesystem (try: mkfs)\n");
        return;
    }

    const size_t elf_len = (size_t)(hello_elf_end - hello_elf_start);
    if (elf_len == 0) {
        print_str("install: no embedded program\n");
        return;
    }

    /* recreate to make the demo idempotent: remove by remounting? crimsonfs
     * has no delete; reuse the file if present */
    if (crimsonfs_create(root, "hello") != 0) {
        /* already exists: reuse and overwrite in place */
    }

    vfs_node_t* f = vfs_open("/hello");
    if (!f) {
        print_set_color(PRINT_COLOR_RED, PRINT_COLOR_BLACK);
        print_str("install: cannot reopen /hello\n");
        return;
    }

    int n = f->write(f, hello_elf_start, elf_len);
    vfs_close(f);

    if (n < 0 || (size_t)n != elf_len) {
        print_set_color(PRINT_COLOR_RED, PRINT_COLOR_BLACK);
        print_str("install: write failed\n");
        return;
    }
    print_set_color(PRINT_COLOR_GREEN, PRINT_COLOR_BLACK);
    print_str("install: wrote hello.elf (");
    print_uint64_dec(elf_len);
    print_str(" bytes) to /\n");
}

static void cmd_exec(const char* name) {
    task_t* t = process_spawn_user(name);
    if (!t) {
        print_set_color(PRINT_COLOR_RED, PRINT_COLOR_BLACK);
        print_str("\nexec: spawn failed\n");
        return;
    }
    print_set_color(PRINT_COLOR_CYAN, PRINT_COLOR_BLACK);
    print_str("\nexec: pid ");
    print_uint64_dec(t->id);
    print_str(" queued\n");
}

void execute_command(char* cmd) {
    if (cmd[0] == '\0') return;

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "HELP") == 0) {
        print_set_color(PRINT_COLOR_RED, PRINT_COLOR_BLACK);
        print_str("cxsh commands:\n");
        print_str("  help    - Show available commands\n");
        print_str("  clear   - Clear terminal screen\n");
        print_str("  reboot  - Restart the CPU\n");
        print_str("  echo    - Print text back to screen\n");
        print_str("  meminfo - Display physical memory statistics\n");
        print_str("  alloc   - Test heap memory allocation\n");
        print_str("  uptime  - Display system uptime\n");
        print_str("  sleep   - Pause shell execution (ms)\n");
        print_str("  crash   - Trigger a CPU exception: crash <pf|gp|ud|de>\n");
        print_str("  vmtest  - Check per-process address spaces\n");
        print_str("  ps      - List processes\n");
        print_str("\nstorage:\n");
        print_str("  ls      - List files on the mounted CrimsonFS\n");
        print_str("  cat     - cat <file>: dump a file's bytes\n");
        print_str("  rm      - rm <file>: remove a file from CrimsonFS\n");
        print_str("  mkfs    - Format disk 0 as CrimsonFS and mount it\n");
        print_str("  install - Write the embedded hello.elf program to disk\n");
        print_str("  exec    - exec <path>: load ELF from CrimsonFS, spawn ring-3 task\n");

    } else if (strcmp(cmd, "uptime") == 0 || strcmp(cmd, "UPTIME") == 0) {
        uint64_t uptime_sec = pit_get_uptime_sec();
        uint64_t ticks = pit_get_ticks();

        print_set_color(PRINT_COLOR_CYAN, PRINT_COLOR_BLACK);
        print_str("System Uptime: ");
        print_uint64_dec(uptime_sec);
        print_str(" seconds (");
        print_uint64_dec(ticks);
        print_str(" ticks)\n");

    } else if (strcmp(cmd, "sleep") == 0 || strcmp(cmd, "SLEEP") == 0) {
        print_set_color(PRINT_COLOR_RED, PRINT_COLOR_BLACK);
        print_str("Usage: sleep <milliseconds>\n");

    } else if (cmd[0] == 's' && cmd[1] == 'l' && cmd[2] == 'e' && cmd[3] == 'e' && cmd[4] == 'p' && cmd[5] == ' ') {
        uint32_t ms = 0;
        int i = 6;
        while (cmd[i] >= '0' && cmd[i] <= '9') {
            ms = ms * 10 + (cmd[i] - '0');
            i++;
        }

        if (ms > 0) {
            print_set_color(PRINT_COLOR_YELLOW, PRINT_COLOR_BLACK);
            print_str("Sleeping for ");
            print_uint64_dec(ms);
            print_str(" ms...\n");

            process_sleep(ms);

            print_set_color(PRINT_COLOR_GREEN, PRINT_COLOR_BLACK);
            print_str("Done.\n");
        } else {
            print_set_color(PRINT_COLOR_RED, PRINT_COLOR_BLACK);
            print_str("Usage: sleep <milliseconds>\n");
        }

    } else if (strcmp(cmd, "clear") == 0 || strcmp(cmd, "CLEAR") == 0) {
        print_clear();

    } else if (strcmp(cmd, "reboot") == 0 || strcmp(cmd, "REBOOT") == 0) {
        print_set_color(PRINT_COLOR_RED, PRINT_COLOR_BLACK);
        print_str("Rebooting CrimsonX...\n");
        sys_reboot();

    } else if (strcmp(cmd, "meminfo") == 0 || strcmp(cmd, "MEMINFO") == 0) {
        uint64_t free_blocks = pmm_get_free_block_count();
        uint64_t used_blocks = pmm_get_used_block_count();
        uint64_t total_blocks = free_blocks + used_blocks;

        uint64_t total_mb = (total_blocks * 4096) / (1024 * 1024);
        uint64_t used_mb = (used_blocks * 4096) / (1024 * 1024);
        uint64_t free_mb = (free_blocks * 4096) / (1024 * 1024);

        print_set_color(PRINT_COLOR_CYAN, PRINT_COLOR_BLACK);
        print_str("Physical Memory (PMM) Status:\n");
        print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK);

        print_str("  Block Size:   4 KB\n");
        print_str("  Total Blocks: "); print_uint64_dec(total_blocks); print_str("\n");
        print_str("  Used Blocks:  "); print_uint64_dec(used_blocks); print_str("\n");
        print_str("  Free Blocks:  "); print_uint64_dec(free_blocks); print_str("\n\n");

        print_str("  Total RAM:    "); print_uint64_dec(total_mb); print_str(" MB\n");
        print_str("  Used RAM:     "); print_uint64_dec(used_mb); print_str(" MB\n");
        print_str("  Free RAM:     "); print_uint64_dec(free_mb); print_str(" MB\n");

    } else if (strcmp(cmd, "alloc") == 0 || strcmp(cmd, "ALLOC") == 0) {
        print_set_color(PRINT_COLOR_YELLOW, PRINT_COLOR_BLACK);
        print_str("Testing kmalloc heap allocation...\n");

        char* str = (char*) kmalloc(64);
        if (str != NULL) {
            str[0] = 'H'; str[1] = 'e'; str[2] = 'a'; str[3] = 'p';
            str[4] = ' '; str[5] = 'O'; str[6] = 'K'; str[7] = '\0';

            print_set_color(PRINT_COLOR_GREEN, PRINT_COLOR_BLACK);
            print_str("  Allocated 64 bytes at address: 0x");
            print_uint64_dec((uint64_t)str);
            print_str("\n  Buffer content: ");
            print_str(str);
            print_str("\n");

            kfree(str);
            print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK);
            print_str("  Memory freed successfully.\n");

        } else {
            print_set_color(PRINT_COLOR_RED, PRINT_COLOR_BLACK);
            print_str("  Allocation failed!\n");
        }

    } else if (strcmp(cmd, "ps") == 0 || strcmp(cmd, "PS") == 0) {
        process_print_list();

    } else if (strcmp(cmd, "vmtest") == 0) {
        cmd_vmtest();

    } else if (strcmp(cmd, "crash pf") == 0) {
        *(volatile uint64_t*)0x10000000000ULL = 1;

    } else if (strcmp(cmd, "crash gp") == 0) {
        *(volatile uint64_t*)0x8000000000000000ULL = 1;

    } else if (strcmp(cmd, "crash ud") == 0) {
        asm volatile("ud2");

    } else if (strcmp(cmd, "crash de") == 0) {
        asm volatile("xor %%edx, %%edx\n\tmov $1, %%eax\n\txor %%ecx, %%ecx\n\tdiv %%ecx" : : : "eax", "ecx", "edx");

    } else if (cmd[0] == 'e' && cmd[1] == 'c' && cmd[2] == 'h' && cmd[3] == 'o' && cmd[4] == ' ') {
        print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK);
        print_str(cmd + 5);
        print_str("\n");

    } else if (strcmp(cmd, "mkfs") == 0) {
        cmd_mkfs();

    } else if (strcmp(cmd, "ls") == 0 || strcmp(cmd, "LS") == 0) {
        cmd_ls();

    } else if (cmd[0] == 'c' && cmd[1] == 'a' && cmd[2] == 't' && cmd[3] == ' ') {
        cmd_cat(cmd + 4);

    } else if (cmd[0] == 'r' && cmd[1] == 'm' && cmd[2] == ' ') {
        cmd_rm(cmd + 3);

    } else if (strcmp(cmd, "install") == 0) {
        cmd_install();

    } else if (cmd[0] == 'e' && cmd[1] == 'x' && cmd[2] == 'e' && cmd[3] == 'c' && cmd[4] == ' ') {
        /* trim filepath spaces */
        char* path = cmd + 5;
        size_t len = strlen(path);
        while (len > 0 && (path[len - 1] == ' ')) {
            len--;
            path[len] = '\0';
        }
        if (len == 0) {
            print_set_color(PRINT_COLOR_RED, PRINT_COLOR_BLACK);
            print_str("Usage: exec <path>\n");
        } else {
            cmd_exec(path);
        }

    } else {
        print_set_color(PRINT_COLOR_RED, PRINT_COLOR_BLACK);
        print_str("cxsh: command not found: ");
        print_str(cmd);
        print_str("\n");
    }
}
