#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include <stddef.h>

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
     * NOTE: switch.asm hard-codes offset 8 for `rsp`, so keep id/rsp first. */
    uint64_t kernel_stack_top;
} task_t;

void process_init(void);
task_t* process_create(void (*entry_point)(void));
void process_sleep(uint32_t ms);
void process_exit(void);
void process_print_list(void);
void schedule(void);

#endif