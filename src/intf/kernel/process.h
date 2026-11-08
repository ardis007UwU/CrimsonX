#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include <stddef.h>
#include "fs/vfs.h"
#include "kernel/signal.h"

struct syscall_frame;
struct vm_mapping;

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
    const void* wait_channel;
    struct task* parent;
    int32_t exit_code;
    uint64_t heap_start;
    uint64_t heap_end;
    uint64_t heap_limit;
    uint64_t mmap_base;
    uint64_t mmap_next;
    struct vm_mapping* mappings;
    char cwd[VFS_PATH_MAX];
    uint64_t pending_signals;
    uint64_t signal_handlers[KERNEL_SIGNAL_MAX + 1];

    /* Ring-3 state used by the exec/spawn machinery (exec.c) */
    uint64_t user_entry;      /* ring-3 entry point (syscall ABI-safe) */
    uint64_t user_stack;      /* ring-3 stack top for iret frames */
    void* spawn_arg;          /* kmalloc'd path string owned by this task */

    /* Per-task file descriptor table (0 = stdin, 1 = stdout, 2 = stderr) */
    vfs_file_t* fds[VFS_FD_MAX];
} task_t;

/** Initialize the boot task and scheduler state. */
void process_init(void);
/** Create and enqueue a kernel task; returns NULL if allocation fails. */
task_t* process_create(void (*entry_point)(void));
/** Block the current task for the requested number of milliseconds. */
void process_sleep(uint32_t ms);
/** Terminate the current task with status zero and never return. */
void process_exit(void);
/** Print the scheduler's task list to the console. */
void process_print_list(void);
/** Schedule another runnable task if one is available. */
void schedule(void);
/** Enter ring 3 at entry_point with the supplied user stack. */
void process_enter_user(void* entry_point, void* user_stack_top);

/** @return The currently running task, or NULL before scheduler setup. */
task_t* process_current_task(void);
/** Park the current task until explicitly woken. */
void process_block(void);
/** Park the current task on a wait channel until that channel is woken. */
void process_block_on(const void* channel);
/** Make a blocked task runnable without immediately rescheduling. */
void process_wakeup(task_t* task);
/** Wake tasks parked on the specified channel. */
void process_wakeup_channel(const void* channel);
/** Release current-task resources, record status, and switch away. */
void process_kill_current(int32_t exit_code);
/** Clone the current user task, including its user mappings and descriptors. */
int64_t process_fork(struct syscall_frame* frame);
/**
 * Wait for a matching child, block until it exits, and reap it.
 * @param status Receives the child's raw exit code, if non-NULL.
 * @return Reaped child PID, or -1 if there is no matching child.
 */
int64_t process_waitpid(int64_t pid, int32_t* status);
/**
 * Install descriptors 0 (keyboard), 1 (console), and 2 (console).
 * Rejects occupied tables; allocation failure leaves an initially empty table.
 */
int process_fd_defaults(task_t* task);
/** Queue a supported signal for a task PID; signal zero probes for existence. */
int process_send_signal(int64_t pid, uint32_t signal);
/** Deliver one pending default-action signal to the current task. */
void process_handle_pending_signal(void);
/** Select the process interrupted by shell Ctrl+C; PID zero clears it. */
void process_set_foreground(uint64_t pid);
/** @return The current foreground PID, or zero when none is selected. */
uint64_t process_get_foreground(void);

#endif
