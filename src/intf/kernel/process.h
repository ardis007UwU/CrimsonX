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
} task_t;

void process_sleep(uint32_t ms);
task_t* process_create(void (*entry_point)(void));
void schedule(void);

#endif