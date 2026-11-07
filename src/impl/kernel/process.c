#include "kernel/process.h"
#include "mm/kmalloc.h"
#include "print.h"
#include "x86_64/pit.h"

#define STACK_SIZE 4096

static task_t* current_task = NULL;
static task_t* task_list_head = NULL;
static uint64_t next_pid = 1;

extern void switch_to(task_t* next, task_t* current);

void process_init(void) {
    task_t* main_task = (task_t*) kmalloc(sizeof(task_t));
    main_task->id = 0;
    main_task->state = TASK_STATE_RUNNING;
    main_task->cr3 = 0;
    main_task->wake_ticks = 0;
    main_task->kernel_stack = NULL;
    main_task->next = main_task;

    current_task = main_task;
    task_list_head = main_task;
}

task_t* process_create(void (*entry_point)(void)) {
    task_t* new_task = (task_t*) kmalloc(sizeof(task_t));
    if (!new_task) return NULL;

    new_task->id = next_pid++;
    new_task->state = TASK_STATE_READY;
    new_task->wake_ticks = 0;
    new_task->kernel_stack = kmalloc(STACK_SIZE);
    
    if (!new_task->kernel_stack) {
        kfree(new_task);
        return NULL;
    }

    uint64_t* stack = (uint64_t*)((uint64_t)new_task->kernel_stack + STACK_SIZE);

    *(--stack) = (uint64_t)entry_point;

    for (int i = 0; i < 15; i++) {
        *(--stack) = 0;
    }

    new_task->rsp = (uint64_t)stack;

    new_task->next = task_list_head->next;
    task_list_head->next = new_task;

    return new_task;
}

void process_sleep(uint32_t ms) {
    if (!current_task) return;

    uint64_t ticks_to_wait = ((uint64_t)ms * 100) / 1000;
    if (ticks_to_wait == 0 && ms > 0) ticks_to_wait = 1;

    current_task->wake_ticks = pit_get_ticks() + ticks_to_wait;
    current_task->state = TASK_STATE_BLOCKED;

    schedule();
}

void schedule(void) {
    if (!current_task || !current_task->next) return;

    uint64_t current_ticks = pit_get_ticks();
    task_t* curr = task_list_head;

    /* Wake up any sleeping tasks whose wait target has passed */
    do {
        if (curr->state == TASK_STATE_BLOCKED && current_ticks >= curr->wake_ticks) {
            curr->state = TASK_STATE_READY;
        }
        curr = curr->next;
    } while (curr != task_list_head);

    task_t* prev = current_task;
    task_t* next = current_task->next;

    /* Find the next runnable task */
    while (next->state != TASK_STATE_READY && next->state != TASK_STATE_RUNNING) {
        next = next->next;
        if (next == prev) return;
    }

    if (next == prev) return;

    if (prev->state == TASK_STATE_RUNNING) {
        prev->state = TASK_STATE_READY;
    }
    
    next->state = TASK_STATE_RUNNING;
    current_task = next;

    switch_to(next, prev);
}