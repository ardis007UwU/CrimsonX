#include "kernel/process.h"
#include "kernel/irq.h"
#include "mm/kmalloc.h"
#include "print.h"
#include "x86_64/pit.h"
#include "x86_64/gdt.h"
#include "mm/vmm.h"

/* 16 KB: interrupts, syscalls and exception handlers run on this stack */
#define STACK_SIZE 16384

/* switch.asm reads/writes task->rsp at [reg + 8] */
_Static_assert(__builtin_offsetof(task_t, rsp) == 8, "task_t.rsp must stay at offset 8 (switch.asm)");
/* syscall_entry.asm loads the kernel stack from task + 56 */
_Static_assert(__builtin_offsetof(task_t, kernel_stack_top) == 56,
               "task_t.kernel_stack_top must stay at offset 56 (syscall_entry.asm)");

static task_t* task_list_head = NULL;
static uint64_t next_pid = 1;

/* Current task is non-static so syscall_entry.asm can reach the per-task
 * kernel stack. */
task_t* current_task = NULL;

extern void switch_to(task_t* next, task_t* current);
extern uint64_t get_rsp(void);

void process_init(void) {
    task_t* main_task = (task_t*) kmalloc(sizeof(task_t));
    main_task->id = 0;
    main_task->state = TASK_STATE_RUNNING;
    main_task->cr3 = vmm_kernel_cr3();
    main_task->wake_ticks = 0;
    main_task->kernel_stack = NULL;
    main_task->kernel_stack_top = 0;
    main_task->user_entry = 0;
    main_task->user_stack = 0;
    main_task->spawn_arg = NULL;
    for (int i = 0; i < VFS_FD_MAX; i++) main_task->fds[i] = NULL;

    /* Safely fetch exact stack pointer from pure assembly function */
    main_task->rsp = get_rsp();

    main_task->next = main_task;

    current_task = main_task;
    task_list_head = main_task;
}

task_t* process_current_task(void) {
    return current_task;
}

void process_block(void) {
    task_t* t = current_task;
    if (!t) return;
    /* UINT64_MAX wake: only an explicit process_wakeup() can clear us. */
    t->wake_ticks = UINT64_MAX;
    t->state = TASK_STATE_BLOCKED;
    schedule();
}

void process_wakeup(task_t* task) {
    if (task && task->state == TASK_STATE_BLOCKED) {
        task->state = TASK_STATE_READY;
    }
}

void process_fd_defaults(task_t* task) {
    if (!task) return;
    for (int i = 0; i < VFS_FD_MAX; i++) task->fds[i] = NULL;

    vfs_file_t* f;
    f = (vfs_file_t*)kmalloc(sizeof(vfs_file_t));
    if (f) { f->node = vfs_keyboard_channel(); f->offset = 0; f->owns_node = 0; }
    task->fds[VFS_FD_STDIN] = f;

    for (int fd = 1; fd <= 2; fd++) {
        f = (vfs_file_t*)kmalloc(sizeof(vfs_file_t));
        if (f) { f->node = vfs_console_channel(); f->offset = 0; f->owns_node = 0; }
        task->fds[fd] = f;
    }
}

void process_kill_current(void) {
    task_t* t = current_task;
    if (!t) return;

    /* fds: free descriptors (and heap-owned file nodes) */
    for (int i = 0; i < VFS_FD_MAX; i++) {
        vfs_file_t* f = t->fds[i];
        if (!f) continue;
        if (f->node && f->owns_node) vfs_close(f->node);
        kfree(f);
        t->fds[i] = NULL;
    }
    if (t->spawn_arg) {
        kfree(t->spawn_arg);
        t->spawn_arg = NULL;
    }

    /* Free the user address space if this is a ring-3 task. Safe: we are on
     * the kernel stack. vmm_destroy switches CR3 back to the kernel itself. */
    if (t->cr3 && t->cr3 != vmm_kernel_cr3()) {
        vmm_destroy_address_space(t->cr3);
        t->cr3 = vmm_kernel_cr3();
    }

    t->state = TASK_STATE_DEAD;
    schedule();

    /* Nobody left runnable: do not unwind the syscall frame of a dead task. */
    while (1) {
        asm volatile("cli; hlt");
    }
}

void process_exit(void) {
    /* Full teardown handles both kernel tasks (nothing to free) and user
     * tasks (fds, arguments, address space). */
    process_kill_current();
}

task_t* process_create(void (*entry_point)(void)) {
    task_t* new_task = (task_t*) kmalloc(sizeof(task_t));
    if (!new_task) return NULL;

    new_task->id = next_pid++;
    new_task->state = TASK_STATE_READY;
    new_task->wake_ticks = 0;
    new_task->cr3 = vmm_kernel_cr3();
    new_task->kernel_stack = kmalloc(STACK_SIZE);
    new_task->user_entry = 0;
    new_task->user_stack = 0;
    new_task->spawn_arg = NULL;
    for (int i = 0; i < VFS_FD_MAX; i++) new_task->fds[i] = NULL;

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

    /* Reserve space for 15 general-purpose registers (RAX-R15) matching switch_to */
    for (int i = 0; i < 15; i++) {
        *(--stack) = 0;
    }

    new_task->rsp = (uint64_t)stack;

    /* Insert into circular linked list queue */
    uint64_t flags = irq_save();
    new_task->next = task_list_head->next;
    task_list_head->next = new_task;
    irq_restore(flags);

    return new_task;
}

void process_sleep(uint32_t ms) {
    if (!current_task) return;

    uint64_t flags = irq_save();
    /* Your PIT already runs at 1000Hz, so 1 tick = 1 ms */
    current_task->wake_ticks = pit_get_ticks() + ms;
    current_task->state = TASK_STATE_BLOCKED;
    irq_restore(flags);

    schedule();
}

static void schedule_locked(void) {
    if (!current_task || !current_task->next) return;

    uint64_t current_ticks = pit_get_ticks();
    task_t* search = current_task->next;

    /* Unblock sleeping tasks whose wake timer has elapsed */
    while (search != current_task) {
        if (search->state == TASK_STATE_BLOCKED && current_ticks >= search->wake_ticks) {
            search->state = TASK_STATE_READY;
        }
        search = search->next;
    }

    task_t* prev = current_task;
    task_t* next = current_task->next;

    /* Find next runnable task */
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

    /* Per-task hardware state updates */
    if (next->kernel_stack_top) gdt_set_kernel_stack(next->kernel_stack_top);
    vmm_switch(next->cr3);

    switch_to(next, prev);
}

void schedule(void) {
    uint64_t flags = irq_save();
    schedule_locked();
    irq_restore(flags);
}

void process_print_list(void) {
    if (!task_list_head) return;

    uint64_t flags = irq_save();
    print_set_color(PRINT_COLOR_CYAN, PRINT_COLOR_BLACK);
    print_str("PID    STATE         STACK POINTER\n");

    task_t* curr = task_list_head;
    do {
        print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK);
        print_uint64_dec(curr->id);
        print_str("      ");

        switch (curr->state) {
            case TASK_STATE_READY:   print_str("READY       "); break;
            case TASK_STATE_RUNNING: print_str("RUNNING     "); break;
            case TASK_STATE_BLOCKED: print_str("BLOCKED     "); break;
            case TASK_STATE_DEAD:    print_str("DEAD        "); break;
        }

        print_str("  ");
        if (curr->id == 0) {
            print_str("(boot stack)");
        } else {
            print_str("0x");
            print_uint64_hex(curr->rsp);
        }
        print_str("\n");

        curr = curr->next;
    } while (curr != task_list_head);

    irq_restore(flags);
}

void process_enter_user(void* entry_point, void* user_stack_top) {
    asm volatile(
        "cli\n\t"
        "mov $0x1B, %%ax\n\t"    /* GDT_SEL_USER_DATA (0x18 | 3) */
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"

        "pushq $0x1B\n\t"         /* User SS */
        "pushq %0\n\t"            /* User RSP */
        "pushq $0x202\n\t"        /* RFLAGS (IF enabled) */
        "pushq $0x23\n\t"         /* User CS (0x20 | 3) */
        "pushq %1\n\t"            /* User RIP */
        "iretq"
        :
        : "r"(user_stack_top), "r"(entry_point)
        : "memory"
    );
}