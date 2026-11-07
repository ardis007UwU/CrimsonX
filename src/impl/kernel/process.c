#include "kernel/process.h"
#include "mm/kmalloc.h"
#include "print.h"
#include "x86_64/pit.h"
#include "x86_64/gdt.h"
#include "mm/vmm.h"

/* 16 KB: interrupts, syscalls and the register dump from exception_handler
 * all run on this stack, 4 KB was too tight. */
#define STACK_SIZE 16384

/* switch.asm reads/writes task->rsp at [reg + 8] */
_Static_assert(__builtin_offsetof(task_t, rsp) == 8, "task_t.rsp must stay at offset 8 (switch.asm)");

#define RFLAGS_IF (1ull << 9)

/* Interrupt-flag save/restore. schedule() is called both from the timer IRQ
 * (IF already 0) and from task context (process_sleep, IF = 1). */
static inline uint64_t irq_save(void) {
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags) {
    if (flags & RFLAGS_IF) asm volatile("sti" : : : "memory");
}

static task_t* current_task = NULL;
static task_t* task_list_head = NULL;
static uint64_t next_pid = 1;

extern void switch_to(task_t* next, task_t* current);
extern uint64_t get_rsp(void);

void process_exit(void) {
    if (!current_task) return;

    current_task->state = TASK_STATE_DEAD;
    schedule();

    /* Safety loop in case scheduler returns to a dead task */
    while (1) {
        asm volatile("hlt");
    }
}

void process_init(void) {
    task_t* main_task = (task_t*) kmalloc(sizeof(task_t));
    main_task->id = 0;
    main_task->state = TASK_STATE_RUNNING;
    main_task->cr3 = vmm_kernel_cr3();
    main_task->wake_ticks = 0;
    main_task->kernel_stack = NULL;
    main_task->kernel_stack_top = 0;

    /* Safely fetch exact stack pointer from pure assembly function */
    main_task->rsp = get_rsp();

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
    new_task->cr3 = vmm_kernel_cr3();
    new_task->kernel_stack = kmalloc(STACK_SIZE);

    if (!new_task->kernel_stack) {
        kfree(new_task);
        return NULL;
    }

    /* kmalloc hands out block + 24 bytes, so base + STACK_SIZE is only 8-byte
     * aligned. Round the top down to 16: the task must start with
     * RSP % 16 == 8, as if it had been called. */
    uint64_t stack_top = ((uint64_t)new_task->kernel_stack + STACK_SIZE) & ~0xFULL;
    new_task->kernel_stack_top = stack_top;

    uint64_t* stack = (uint64_t*)stack_top;

    /* Push exit wrapper: if entry_point returns, jump directly to process_exit */
    *(--stack) = (uint64_t)process_exit;
    *(--stack) = (uint64_t)entry_point;

    /* Reserve space for 15 general-purpose registers (RAX-R15) */
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

static void schedule_locked(void) {
    if (!current_task || !current_task->next) return;

    uint64_t current_ticks = pit_get_ticks();
    task_t* curr = task_list_head;

    /* Wake up any sleeping tasks whose wait timer has expired */
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

    /* Per-task hardware state. Both are safe to change while still on prev's
     * stack: every kernel stack lives in the kernel half, which is mapped in
     * every address space. */
    if (next->kernel_stack_top) gdt_set_kernel_stack(next->kernel_stack_top);
    vmm_switch(next->cr3);

    switch_to(next, prev);
}

/* Interrupts stay off for the whole switch so a timer tick cannot re-enter the
 * scheduler between updating rsp0/cr3 and the actual stack switch. The saved IF
 * is restored when this task resumes, or immediately if nothing was switched. */
void schedule(void) {
    uint64_t flags = irq_save();
    schedule_locked();
    irq_restore(flags);
}

void process_print_list(void) {
    if (!task_list_head) return;

    print_set_color(PRINT_COLOR_CYAN, PRINT_COLOR_BLACK);
    print_str("PID   STATE       STACK POINTER\n");
    print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK);

    task_t* curr = task_list_head;
    do {
        print_uint64_dec(curr->id);
        print_str("     ");

        if (curr->state == TASK_STATE_RUNNING) print_str("RUNNING     ");
        else if (curr->state == TASK_STATE_READY) print_str("READY       ");
        else if (curr->state == TASK_STATE_BLOCKED) print_str("BLOCKED     ");
        else print_str("DEAD        ");

        if (curr->id == 0) {
            print_str("(boot stack)\n");
        } else {
            print_str("0x");
            print_uint64_hex((uint64_t)curr->kernel_stack);
            print_str("\n");
        }

        curr = curr->next;
    } while (curr != task_list_head);
}