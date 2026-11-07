#include "commands.h"
#include "print.h"
#include "string.h"
#include "x86_64/port.h"
#include "x86_64/pit.h"
#include "mm/pmm.h"
#include "mm/kmalloc.h"
#include "kernel/process.h"

static void sys_reboot(void) {
    uint8_t status = 0x02;
    while (status & 0x02) {
        status = port_inb(0x64);
    }
    port_outb(0x64, 0xFE);
    asm volatile("cli; hlt");
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

            pit_sleep(ms);

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
    
    } else if (cmd[0] == 'e' && cmd[1] == 'c' && cmd[2] == 'h' && cmd[3] == 'o' && cmd[4] == ' ') {
        print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK);
        print_str(cmd + 5);
        print_str("\n");
        
        
    } else {
        print_set_color(PRINT_COLOR_RED, PRINT_COLOR_BLACK);
        print_str("cxsh: command not found: ");
        print_str(cmd);
        print_str("\n");
    }
}