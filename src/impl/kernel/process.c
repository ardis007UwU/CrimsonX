#include "kernel/process.h"
#include "kernel/irq.h"
#include "mm/kmalloc.h"
#include "print.h"
#include "x86_64/pit.h"
#include "x86_64/gdt.h"
#include "mm/vmm.h"
#include "x86_64/syscall.h"
#include "kernel/mmap.h"

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
extern void fork_child_start(void);

void process_init(void) {
    task_t* main_task = (task_t*) kmalloc(sizeof(task_t));
    if (!main_task) {
        asm volatile("cli");
        for (;;) asm volatile("hlt");
    }
    main_task->id = 0;
    main_task->state = TASK_STATE_RUNNING;
    main_task->cr3 = vmm_kernel_cr3();
    main_task->wake_ticks = 0;
    main_task->wait_channel = NULL;
    main_task->kernel_stack = NULL;
    main_task->kernel_stack_top = 0;
    main_task->user_entry = 0;
    main_task->user_stack = 0;
    main_task->spawn_arg = NULL;
    main_task->parent = NULL;
    main_task->exit_code = 0;
    main_task->heap_start = 0;
    main_task->heap_end = 0;
    main_task->heap_limit = 0;
    main_task->mmap_base = 0;
    main_task->mmap_next = 0;
    main_task->mappings = NULL;
    for (int i = 0; i < VFS_FD_MAX; i++) main_task->fds[i] = NULL;

    if (process_fd_defaults(main_task) != 0) {
        kfree(main_task);
        asm volatile("cli");
        for (;;) asm volatile("hlt");
    }

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
    uint64_t flags = irq_save();
    task_t* t = current_task;
    if (!t) {
        irq_restore(flags);
        return;
    }
    /* UINT64_MAX wake: only an explicit process_wakeup() can clear us. */
    t->wake_ticks = UINT64_MAX;
    t->wait_channel = NULL;
    t->state = TASK_STATE_BLOCKED;
    schedule();
    irq_restore(flags);
}

void process_block_on(const void* channel) {
    if (!channel) {
        process_block();
        return;
    }
    uint64_t flags = irq_save();
    task_t* t = current_task;
    if (!t) {
        irq_restore(flags);
        return;
    }
    t->wake_ticks = UINT64_MAX;
    t->wait_channel = channel;
    t->state = TASK_STATE_BLOCKED;
    schedule();
    irq_restore(flags);
}

void process_wakeup(task_t* task) {
    uint64_t flags = irq_save();
    if (task && task->state == TASK_STATE_BLOCKED) {
        task->wait_channel = NULL;
        task->state = TASK_STATE_READY;
    }
    irq_restore(flags);
}

void process_wakeup_channel(const void* channel) {
    if (!channel || !task_list_head) return;
    uint64_t flags = irq_save();
    task_t* task = task_list_head;
    do {
        if (task->state == TASK_STATE_BLOCKED && task->wait_channel == channel) {
            task->wait_channel = NULL;
            task->state = TASK_STATE_READY;
        }
        task = task->next;
    } while (task && task != task_list_head);
    irq_restore(flags);
}

int process_fd_defaults(task_t* task) {
    if (!task) return -1;
    for (int i = 0; i < VFS_FD_MAX; i++) {
        if (task->fds[i]) return -1;
    }

    for (int fd = VFS_FD_STDIN; fd <= VFS_FD_STDERR; fd++) {
        vfs_file_t* file = (vfs_file_t*)kmalloc(sizeof(vfs_file_t));
        if (!file) {
            for (int i = VFS_FD_STDIN; i < fd; i++) {
                kfree(task->fds[i]);
                task->fds[i] = NULL;
            }
            return -1;
        }

        file->node = (fd == VFS_FD_STDIN) ? vfs_keyboard_channel() : vfs_console_channel();
        file->offset = 0;
        file->owns_node = 0;
        file->access_mode = (fd == VFS_FD_STDIN) ? VFS_ACCESS_READ : VFS_ACCESS_WRITE;
        file->references = 1;
        task->fds[fd] = file;
    }
    return 0;
}

void process_kill_current(int32_t exit_code) {
    uint64_t flags = irq_save();
    task_t* t = current_task;
    if (!t) {
        irq_restore(flags);
        return;
    }

    for (int i = 0; i < VFS_FD_MAX; i++) {
        vfs_file_t* f = t->fds[i];
        if (!f) continue;
        vfs_file_release(f);
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
    vm_mmap_destroy(t);

    for (task_t* child = task_list_head; child; child = child->next) {
        if (child->parent == t) {
            child->parent = (task_list_head != t) ? task_list_head : NULL;
        }
        if (child->next == task_list_head) break;
    }

    t->exit_code = exit_code;
    t->state = TASK_STATE_DEAD;
    if (t->parent) process_wakeup(t->parent);
    irq_restore(flags);
    schedule();

    /* Nobody left runnable: do not unwind the syscall frame of a dead task. */
    while (1) {
        asm volatile("cli; hlt");
    }
}

void process_exit(void) {
    /* Full teardown handles both kernel tasks (nothing to free) and user
     * tasks (fds, arguments, address space). */
    process_kill_current(0);
}

task_t* process_create(void (*entry_point)(void)) {
    if (!entry_point) return NULL;
    task_t* new_task = (task_t*) kmalloc(sizeof(task_t));
    if (!new_task) return NULL;

    new_task->id = 0;
    new_task->state = TASK_STATE_READY;
    new_task->wake_ticks = 0;
    new_task->wait_channel = NULL;
    new_task->cr3 = vmm_kernel_cr3();
    new_task->kernel_stack = kmalloc(STACK_SIZE);
    new_task->user_entry = 0;
    new_task->user_stack = 0;
    new_task->spawn_arg = NULL;
    new_task->parent = current_task;
    new_task->exit_code = 0;
    new_task->heap_start = 0;
    new_task->heap_end = 0;
    new_task->heap_limit = 0;
    new_task->mmap_base = 0;
    new_task->mmap_next = 0;
    new_task->mappings = NULL;
    for (int i = 0; i < VFS_FD_MAX; i++) new_task->fds[i] = NULL;

    if (!new_task->kernel_stack) {
        kfree(new_task);
        return NULL;
    }
    if (process_fd_defaults(new_task) != 0) {
        kfree(new_task->kernel_stack);
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
    if (!task_list_head) {
        irq_restore(flags);
        for (int i = 0; i < VFS_FD_MAX; i++) {
            kfree(new_task->fds[i]);
        }
        kfree(new_task->kernel_stack);
        kfree(new_task);
        return NULL;
    }
    new_task->id = next_pid++;
    new_task->next = task_list_head->next;
    task_list_head->next = new_task;
    irq_restore(flags);

    return new_task;
}

int64_t process_fork(struct syscall_frame* frame) {
    task_t* parent = current_task;
    if (!parent || !frame || !parent->cr3 || parent->cr3 == vmm_kernel_cr3()) return -1;

    uint64_t child_cr3 = vmm_clone_address_space(parent->cr3);
    if (!child_cr3) return -1;

    task_t* child = (task_t*)kmalloc(sizeof(task_t));
    if (!child) {
        vmm_destroy_address_space(child_cr3);
        return -1;
    }
    child->kernel_stack = kmalloc(STACK_SIZE);
    if (!child->kernel_stack) {
        kfree(child);
        vmm_destroy_address_space(child_cr3);
        return -1;
    }

    child->cr3 = child_cr3;
    child->state = TASK_STATE_READY;
    child->wake_ticks = 0;
    child->wait_channel = NULL;
    child->parent = parent;
    child->exit_code = 0;
    child->heap_start = parent->heap_start;
    child->heap_end = parent->heap_end;
    child->heap_limit = parent->heap_limit;
    child->mmap_base = parent->mmap_base;
    child->mmap_next = parent->mmap_next;
    child->mappings = NULL;
    child->user_entry = parent->user_entry;
    child->user_stack = parent->user_stack;
    child->spawn_arg = NULL;
    child->next = NULL;
    for (int i = 0; i < VFS_FD_MAX; i++) child->fds[i] = NULL;

    for (int i = 0; i < VFS_FD_MAX; i++) {
        if (!parent->fds[i]) continue;
        if (vfs_file_retain(parent->fds[i]) != 0) {
            for (int j = 0; j < i; j++) {
                if (child->fds[j]) vfs_file_release(child->fds[j]);
            }
            kfree(child->kernel_stack);
            kfree(child);
            vmm_destroy_address_space(child_cr3);
            return -1;
        }
        child->fds[i] = parent->fds[i];
    }
    if (vm_mmap_clone(parent, child) != 0) {
        for (int i = 0; i < VFS_FD_MAX; i++) {
            if (child->fds[i]) vfs_file_release(child->fds[i]);
        }
        vm_mmap_destroy(child);
        kfree(child->kernel_stack);
        kfree(child);
        vmm_destroy_address_space(child_cr3);
        return -1;
    }

    uint64_t stack_top = ((uint64_t)child->kernel_stack + STACK_SIZE) & ~0xFULL;
    child->kernel_stack_top = stack_top;
    uint64_t frame_address = stack_top - sizeof(struct syscall_frame);
    struct syscall_frame* child_frame = (struct syscall_frame*)(uintptr_t)frame_address;
    const uint8_t* source_frame = (const uint8_t*)frame;
    uint8_t* destination_frame = (uint8_t*)child_frame;
    for (size_t i = 0; i < sizeof(*frame); i++) destination_frame[i] = source_frame[i];

    uint64_t* context = (uint64_t*)(uintptr_t)(frame_address - 16 * sizeof(uint64_t));
    for (size_t i = 0; i < 15; i++) context[i] = 0;
    context[9] = frame_address;
    context[15] = (uint64_t)(uintptr_t)fork_child_start;
    child->rsp = (uint64_t)(uintptr_t)context;

    uint64_t flags = irq_save();
    if (!task_list_head || current_task != parent) {
        irq_restore(flags);
        for (int i = 0; i < VFS_FD_MAX; i++) {
            if (child->fds[i]) vfs_file_release(child->fds[i]);
        }
        vm_mmap_destroy(child);
        kfree(child->kernel_stack);
        kfree(child);
        vmm_destroy_address_space(child_cr3);
        return -1;
    }
    child->id = next_pid++;
    child->next = task_list_head->next;
    task_list_head->next = child;
    irq_restore(flags);
    return (int64_t)child->id;
}

int64_t process_waitpid(int64_t pid, int32_t* status) {
    task_t* parent = current_task;
    if (!parent || pid == 0 || pid < -1) return -1;

    for (;;) {
        uint64_t flags = irq_save();
        if (!task_list_head) {
            irq_restore(flags);
            return -1;
        }

        task_t* previous = task_list_head;
        task_t* candidate = task_list_head;
        task_t* candidate_previous = NULL;
        do {
            if (candidate->parent == parent &&
                (pid == -1 || candidate->id == (uint64_t)pid)) {
                candidate_previous = previous;
                break;
            }
            previous = candidate;
            candidate = candidate->next;
        } while (candidate != task_list_head);

        if (!candidate_previous) {
            irq_restore(flags);
            return -1;
        }

        if (candidate->state == TASK_STATE_DEAD) {
            uint64_t child_pid = candidate->id;
            int32_t child_status = candidate->exit_code;
            candidate_previous->next = candidate->next;
            if (task_list_head == candidate) task_list_head = candidate->next;

            for (task_t* child = task_list_head; child; child = child->next) {
                if (child->parent == candidate) child->parent = task_list_head;
                if (child->next == task_list_head) break;
            }
            irq_restore(flags);

            if (candidate->kernel_stack) kfree(candidate->kernel_stack);
            kfree(candidate);
            if (status) *status = child_status;
            return (int64_t)child_pid;
        }

        parent->wake_ticks = UINT64_MAX;
        parent->state = TASK_STATE_BLOCKED;
        schedule();
        irq_restore(flags);
    }
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