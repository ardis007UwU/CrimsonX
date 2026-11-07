#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include <stddef.h>
#include "fs/vfs.h"

typedef enum {
    TASK_STATE_READY,
    TASK_STATE_RUNNING,
    TASK_STATE_BLOCKED,
    TASK_STATE_DEAD
} task_state_t;

typedef struct task {
    uint64_t id;
    uint64_t rsp;
    uint64_t cr3;
    task_state_t state;
    uint64_t wake_ticks;
    void* kernel_stack;
    struct task* next;
    /* Top of this task's kernel stack (16-byte aligned), loaded into TSS.rsp0 on
     * every switch to the task so ring 3 -> ring 0 transitions land on it.
     * 0 for the boot task, which never runs in ring 3.
     * NOTE: switch.asm hard-codes offset 8 for `rsp`, so keep id/rsp first.
     * syscall_entry.asm hard-codes 56 for kernel_stack_top. */
    uint64_t kernel_stack_top;

    /* Ring-3 state used by the exec/spawn machinery (exec.c) */
    uint64_t user_entry;      /* ring-3 entry point (syscall ABI-safe) */
    uint64_t user_stack;      /* ring-3 stack top for iret frames */
    void* spawn_arg;          /* kmalloc'd path string owned by this task */

    /* Per-task file descriptor table (0 = stdin, 1 = stdout, 2 = stderr) */
    vfs_file_t* fds[VFS_FD_MAX];
} task_t;

void process_init(void);
task_t* process_create(void (*entry_point)(void));
void process_sleep(uint32_t ms);
void process_exit(void);
void process_print_list(void);
void schedule(void);
void process_enter_user(void* entry_point, void* user_stack_top);

/* ---- new primitives ---- */

task_t* process_current_task(void);

/* Park the current task until process_wakeup() (used for blocking I/O,
 * e.g. SYS_READ on the keyboard). Requires IF off; takes the CPU away. */
void process_block(void);

/* IRQ-safe: move a BLOCKED task back to READY (no preemptive reschedule). */
void process_wakeup(task_t* task);

/* Terminate the current task: release fds/address space/args, switch away.
 * Never returns. */
void process_kill_current(void);

/* Attach the default fd set (0 keyboard, 1+2 console) to a task. */
void process_fd_defaults(task_t* task);

#endif
