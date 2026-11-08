#include "x86_64/syscall.h"
#include "x86_64/msr.h"
#include "x86_64/gdt.h"
#include "keyboard.h"
#include "print.h"
#include "kernel/process.h"
#include "fs/vfs.h"
#include "kernel/exec.h"
#include "kernel/elf64.h"
#include "kernel/mmap.h"
#include "drivers/rtc.h"
#include "mm/vmm.h"
#include "mm/kmalloc.h"

extern void syscall_entry(void);

_Static_assert(sizeof(struct syscall_frame) == 144, "syscall frame size must match assembly");
_Static_assert(offsetof(struct syscall_frame, user_rip) == 120,
               "syscall RIP offset must match assembly");

uint64_t syscall_scratch_rsp = 0;
uint64_t syscall_scratch_rax = 0;

#define SYSCALL_IO_MAX 0x100000U
#define SYSCALL_IO_CHUNK 512U
#define USER_PATH_MAX 320U

typedef struct {
    size_t argc;
    size_t envc;
    const char* argv[ELF64_MAX_ARGC];
    const char* envp[ELF64_MAX_ENVC];
    char strings[ELF64_MAX_ARGC + ELF64_MAX_ENVC][ELF64_MAX_ARG_LENGTH];
} syscall_exec_args_t;

void syscall_init(void) {
    uint64_t efer = rdmsr(IA32_EFER);
    wrmsr(IA32_EFER, efer | EFER_SCE);

    uint64_t star = ((uint64_t)GDT_SEL_KERNEL_DATA << 48) |
                    ((uint64_t)GDT_SEL_KERNEL_CODE << 32);
    wrmsr(MSR_STAR, star);
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_SFMASK, 0x200);
}

static int copy_user_memory(void* kernel, uint64_t user, size_t count, int to_user) {
    if (count == 0) return 0;
    if (!kernel || user < USER_SPACE_START || user >= USER_SPACE_END ||
        (uint64_t)count > USER_SPACE_END - user) return -1;

    uint64_t cr3 = vmm_current_cr3();
    size_t copied = 0;
    while (copied < count) {
        uint64_t address = user + copied;
        uint64_t physical = 0;
        uint64_t flags = 0;
        if (vmm_query(cr3, address, &physical, &flags) != 0 ||
            !(flags & PAGE_USER) || (to_user && !(flags & PAGE_WRITE))) {
            return -1;
        }

        size_t in_page = (size_t)(address & (PAGE_SIZE - 1));
        size_t chunk = PAGE_SIZE - in_page;
        if (chunk > count - copied) chunk = count - copied;
        volatile uint8_t* user_bytes = (volatile uint8_t*)(uintptr_t)physical;
        uint8_t* kernel_bytes = (uint8_t*)kernel + copied;
        for (size_t i = 0; i < chunk; i++) {
            if (to_user) user_bytes[i] = kernel_bytes[i];
            else kernel_bytes[i] = user_bytes[i];
        }
        copied += chunk;
    }
    return 0;
}

static int validate_user_range(uint64_t user, size_t count, int write_access) {
    if (count == 0) return 0;
    if (user < USER_SPACE_START || user >= USER_SPACE_END ||
        (uint64_t)count > USER_SPACE_END - user) return -1;

    uint64_t cr3 = vmm_current_cr3();
    uint64_t end = user + count;
    for (uint64_t address = user; address < end;) {
        uint64_t flags = 0;
        if (vmm_query(cr3, address, NULL, &flags) != 0 ||
            !(flags & PAGE_USER) || (write_access && !(flags & PAGE_WRITE))) {
            return -1;
        }
        uint64_t next_page = (address & ~(uint64_t)(PAGE_SIZE - 1)) + PAGE_SIZE;
        address = next_page < end ? next_page : end;
    }
    return 0;
}

static int read_user_string(char* dst, const void* user_string, size_t max) {
    if (!dst || !user_string || max < 2) return -1;
    uint64_t address = (uint64_t)(uintptr_t)user_string;
    for (size_t i = 0; i < max; i++) {
        if (copy_user_memory(&dst[i], address + i, 1, 0) != 0) return -1;
        if (dst[i] == '\0') return 0;
    }
    dst[max - 1] = '\0';
    return -1;
}

static int copy_user_vector(uint64_t user_vector, const char** output,
                            size_t capacity, char* storage,
                            size_t* output_count) {
    if (!output_count || (user_vector && (!output || !storage))) return -1;
    *output_count = 0;
    if (!user_vector) return 0;

    for (size_t i = 0; i <= capacity; i++) {
        if (i > (UINT64_MAX - user_vector) / sizeof(uint64_t)) return -1;
        uint64_t string_address = 0;
        uint64_t slot = user_vector + i * sizeof(uint64_t);
        if (copy_user_memory(&string_address, slot, sizeof(string_address), 0) != 0) {
            return -1;
        }
        if (string_address == 0) {
            *output_count = i;
            return 0;
        }
        if (i == capacity) return -1;

        char* destination = storage + i * ELF64_MAX_ARG_LENGTH;
        if (read_user_string(destination, (const void*)(uintptr_t)string_address,
                             ELF64_MAX_ARG_LENGTH) != 0) return -1;
        output[i] = destination;
    }
    return -1;
}

static int syscall_read(task_t* task, uint64_t raw_fd, uint64_t user_buf, size_t count) {
    if (!task || raw_fd >= VFS_FD_MAX) return -1;
    if (count > SYSCALL_IO_MAX) count = SYSCALL_IO_MAX;
    if (count == 0) return 0;
    if (validate_user_range(user_buf, count, 1) != 0) return -1;

    vfs_file_t* file = task->fds[raw_fd];
    if (!file || !file->node) return -1;

    uint8_t buffer[SYSCALL_IO_CHUNK];
    size_t total = 0;
    while (total < count) {
        size_t chunk = count - total;
        if (chunk > sizeof(buffer)) chunk = sizeof(buffer);

        int n = vfs_file_read(file, buffer, chunk);
        if (n == 0 && raw_fd == VFS_FD_STDIN) {
            n = keyboard_read_blocking((char*)buffer, chunk);
        }
        if (n < 0) return total ? (int)total : -1;
        if (n == 0) break;
        if ((size_t)n > chunk ||
            copy_user_memory(buffer, user_buf + total, (size_t)n, 1) != 0) {
            return total ? (int)total : -1;
        }
        total += (size_t)n;
        if ((size_t)n < chunk) break;
    }
    return (int)total;
}

static int syscall_write(task_t* task, uint64_t raw_fd, uint64_t user_buf, size_t count) {
    if (!task || raw_fd >= VFS_FD_MAX) return -1;
    if (count > SYSCALL_IO_MAX) count = SYSCALL_IO_MAX;
    if (count == 0) return 0;
    if (validate_user_range(user_buf, count, 0) != 0) return -1;

    vfs_file_t* file = task->fds[raw_fd];
    if (!file || !file->node) return -1;

    uint8_t buffer[SYSCALL_IO_CHUNK];
    size_t total = 0;
    while (total < count) {
        size_t chunk = count - total;
        if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
        if (copy_user_memory(buffer, user_buf + total, chunk, 0) != 0) {
            return total ? (int)total : -1;
        }

        int n = vfs_file_write(file, buffer, chunk);
        if (n < 0) return total ? (int)total : -1;
        if (n == 0 || (size_t)n > chunk) break;
        total += (size_t)n;
        if ((size_t)n < chunk) break;
    }
    return (int)total;
}

static int syscall_open(task_t* task, uint64_t user_path, uint64_t raw_mode) {
    if (!task || raw_mode > SYS_OPEN_RDWR) return -1;
    char path[USER_PATH_MAX];
    if (read_user_string(path, (const void*)(uintptr_t)user_path, sizeof(path)) != 0) return -1;

    int fd = -1;
    for (int i = 0; i < VFS_FD_MAX; i++) {
        if (!task->fds[i]) {
            fd = i;
            break;
        }
    }
    if (fd < 0) return -1;

    vfs_node_t* node = vfs_open_at(path, task->cwd);
    if (!node || node->type != VFS_TYPE_FILE) {
        if (node) vfs_close(node);
        return -1;
    }

    vfs_file_t* file = (vfs_file_t*)kmalloc(sizeof(vfs_file_t));
    if (!file) {
        vfs_close(node);
        return -1;
    }
    file->node = node;
    file->offset = 0;
    file->owns_node = 1;
    file->references = 1;
    file->access_mode = raw_mode == SYS_OPEN_RDONLY ? VFS_ACCESS_READ :
                        raw_mode == SYS_OPEN_WRONLY ? VFS_ACCESS_WRITE :
                                        VFS_ACCESS_READ | VFS_ACCESS_WRITE;
    task->fds[fd] = file;
    return fd;
}

static int set_program_break(task_t* task, uint64_t new_break) {
    if (!task || !task->cr3 || task->heap_start == 0 ||
        new_break < task->heap_start || new_break > task->heap_limit) return -1;

    uint64_t old_page_end = (task->heap_end + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t new_page_end = (new_break + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (new_page_end < new_break || new_page_end > task->heap_limit) return -1;

    if (new_page_end > old_page_end) {
        if (vmm_map_user_alloc(task->cr3, old_page_end,
                               (size_t)(new_page_end - old_page_end),
                               PAGE_USER | PAGE_WRITE) != 0) return -1;
    } else if (new_page_end < old_page_end) {
        for (uint64_t page = new_page_end; page < old_page_end; page += PAGE_SIZE) {
            if (vmm_unmap_page_in(task->cr3, page, 1) != 0) return -1;
        }
    }
    task->heap_end = new_break;
    return 0;
}

static int syscall_pipe(task_t* task, uint64_t user_fds) {
    if (!task || validate_user_range(user_fds, 2 * sizeof(int32_t), 1) != 0) return -1;
    int read_fd = -1;
    int write_fd = -1;
    for (int i = 0; i < VFS_FD_MAX; i++) {
        if (task->fds[i]) continue;
        if (read_fd < 0) read_fd = i;
        else {
            write_fd = i;
            break;
        }
    }
    if (write_fd < 0) return -1;

    vfs_node_t* read_node = NULL;
    vfs_node_t* write_node = NULL;
    if (vfs_pipe_create(&read_node, &write_node) != 0) return -1;
    vfs_file_t* reader = (vfs_file_t*)kmalloc(sizeof(*reader));
    vfs_file_t* writer = (vfs_file_t*)kmalloc(sizeof(*writer));
    if (!reader || !writer) {
        kfree(reader);
        kfree(writer);
        vfs_close(read_node);
        vfs_close(write_node);
        return -1;
    }
    reader->node = read_node;
    reader->offset = 0;
    reader->owns_node = 1;
    reader->access_mode = VFS_ACCESS_READ;
    reader->references = 1;
    writer->node = write_node;
    writer->offset = 0;
    writer->owns_node = 1;
    writer->access_mode = VFS_ACCESS_WRITE;
    writer->references = 1;

    int32_t descriptors[2] = { read_fd, write_fd };
    if (copy_user_memory(descriptors, user_fds, sizeof(descriptors), 1) != 0) {
        vfs_file_release(reader);
        vfs_file_release(writer);
        return -1;
    }
    task->fds[read_fd] = reader;
    task->fds[write_fd] = writer;
    return 0;
}

static int syscall_gettimeofday(uint64_t user_timeval) {
    syscall_timeval_t time_value;
    uint64_t seconds = 0;
    if (user_timeval == 0 ||
        validate_user_range(user_timeval, sizeof(time_value), 1) != 0 ||
        rtc_get_unix_time(&seconds) != 0 || seconds > INT64_MAX) return -1;

    time_value.tv_sec = (int64_t)seconds;
    time_value.tv_usec = 0;
    return copy_user_memory(&time_value, user_timeval, sizeof(time_value), 1);
}

uint64_t syscall_handler(struct syscall_frame* frame) {
    if (!frame) return (uint64_t)-1;
    task_t* task = process_current_task();

    switch (frame->rax) {
        case SYS_WRITE_LEGACY: {
            /* Legacy demo ABI: RDI = buffer, RSI = length, always stdout. */
            int written = syscall_write(task, VFS_FD_STDOUT, frame->rdi, (size_t)frame->rsi);
            return written < 0 ? (uint64_t)-1 : (uint64_t)written;
        }

        case SYS_SLEEP:
            process_sleep((uint32_t)frame->rdi);
            return 0;

        case SYS_EXIT:
            process_kill_current((int32_t)frame->rdi);
            for (;;) asm volatile("cli; hlt");

        case SYS_READ: {
            int n = syscall_read(task, frame->rdi, frame->rsi, (size_t)frame->rdx);
            return n < 0 ? (uint64_t)-1 : (uint64_t)n;
        }

        case SYS_YIELD:
            schedule();
            return 0;

        case SYS_EXECVE: {
            char path[USER_PATH_MAX];
            if (!task || !task->kernel_stack_top ||
                read_user_string(path, (const void*)(uintptr_t)frame->rdi, sizeof(path)) != 0) {
                return (uint64_t)-1;
            }
            syscall_exec_args_t* args = (syscall_exec_args_t*)kmalloc(sizeof(*args));
            if (!args) return (uint64_t)-1;
            if (copy_user_vector(frame->rsi, args->argv, ELF64_MAX_ARGC,
                                 &args->strings[0][0], &args->argc) != 0 ||
                copy_user_vector(frame->rdx, args->envp, ELF64_MAX_ENVC,
                                 &args->strings[ELF64_MAX_ARGC][0],
                                 &args->envc) != 0) {
                kfree(args);
                return (uint64_t)-1;
            }
            void* owned_args = args;
            int exec_result = exec_replace_current_args_owned(task, path,
                                                              args->argv, args->argc,
                                                              args->envp, args->envc,
                                                              &owned_args);
            if (owned_args) kfree(owned_args);
            if (exec_result != 0) return (uint64_t)-1;
            return (uint64_t)-1;
        }

        case SYS_OPEN:
            return (uint64_t)syscall_open(task, frame->rdi, frame->rsi);

        case SYS_CLOSE: {
            if (!task || frame->rdi >= VFS_FD_MAX || !task->fds[frame->rdi]) {
                return (uint64_t)-1;
            }
            vfs_file_t* file = task->fds[frame->rdi];
            task->fds[frame->rdi] = NULL;
            int result = vfs_file_release(file);
            return result == 0 ? 0 : (uint64_t)-1;
        }

        case SYS_LSEEK: {
            if (!task || frame->rdi >= VFS_FD_MAX || !task->fds[frame->rdi]) {
                return (uint64_t)-1;
            }
            if (frame->rdx > VFS_SEEK_END) return (uint64_t)-1;
            vfs_file_t* file = task->fds[frame->rdi];
            if (vfs_file_seek(file, (int64_t)frame->rsi, (int)frame->rdx) != 0) {
                return (uint64_t)-1;
            }
            return file->offset;
        }

        case SYS_WRITE: {
            int n = syscall_write(task, frame->rdi, frame->rsi, (size_t)frame->rdx);
            return n < 0 ? (uint64_t)-1 : (uint64_t)n;
        }

        case SYS_STAT: {
            char path[USER_PATH_MAX];
            syscall_stat_t status;
            if (read_user_string(path, (const void*)(uintptr_t)frame->rdi, sizeof(path)) != 0) {
                return (uint64_t)-1;
            }
            if (!task) return (uint64_t)-1;
            vfs_node_t* node = vfs_open_at(path, task->cwd);
            if (!node) return (uint64_t)-1;
            if (node->type == VFS_TYPE_DIR) {
                status.st_mode = SYS_S_IFDIR | 0755U;
            } else if (node->type == VFS_TYPE_FILE) {
                status.st_mode = SYS_S_IFREG | 0644U;
            } else {
                vfs_close(node);
                return (uint64_t)-1;
            }
            status.st_size = (uint32_t)node->size;
            int copy_result = copy_user_memory((void*)&status, frame->rsi,
                                               sizeof(status), 1);
            vfs_close(node);
            return copy_result == 0 ? 0 : (uint64_t)-1;
        }

        case SYS_FORK:
            return (uint64_t)process_fork(frame);

        case SYS_WAITPID: {
            if (frame->rdx != 0 ||
                (frame->rsi && validate_user_range(frame->rsi, sizeof(int32_t), 1) != 0)) {
                return (uint64_t)-1;
            }
            int32_t status = 0;
            int64_t child_pid = process_waitpid((int64_t)frame->rdi,
                                                frame->rsi ? &status : NULL);
            if (child_pid < 0) return (uint64_t)-1;
            status = (int32_t)(((uint32_t)status & 0xFFU) << 8);
            if (frame->rsi && copy_user_memory(&status, frame->rsi,
                                               sizeof(status), 1) != 0) {
                return (uint64_t)-1;
            }
            return (uint64_t)child_pid;
        }

        case SYS_BRK:
            if (!task || !task->heap_start) return (uint64_t)-1;
            if (frame->rdi == 0) return task->heap_end;
            if (set_program_break(task, frame->rdi) != 0) return task->heap_end;
            return task->heap_end;

        case SYS_SBRK: {
            if (!task || !task->heap_start) return (uint64_t)-1;
            uint64_t previous = task->heap_end;
            int64_t increment = (int64_t)frame->rdi;
            uint64_t requested = previous;
            if (increment >= 0) {
                if ((uint64_t)increment > UINT64_MAX - previous) return (uint64_t)-1;
                requested += (uint64_t)increment;
            } else {
                uint64_t magnitude = 0 - (uint64_t)increment;
                if (magnitude > previous - task->heap_start) return (uint64_t)-1;
                requested -= magnitude;
            }
            if (set_program_break(task, requested) != 0) return (uint64_t)-1;
            return previous;
        }

        case SYS_MMAP: {
            if (!task || frame->rsi == 0 || frame->rsi > VM_MMAP_MAX_SIZE ||
                frame->rdx > (SYS_PROT_READ | SYS_PROT_WRITE | SYS_PROT_EXEC) ||
                frame->r10 > (SYS_MAP_SHARED | SYS_MAP_PRIVATE | SYS_MAP_ANONYMOUS) ||
                frame->r9 > INT64_MAX) return (uint64_t)-1;
            int flags = (int)frame->r10;
            vfs_file_t* file = NULL;
            if (!(flags & SYS_MAP_ANONYMOUS)) {
                if (frame->r8 >= VFS_FD_MAX || !task->fds[frame->r8]) {
                    return (uint64_t)-1;
                }
                file = task->fds[frame->r8];
            }
            uint64_t mapped = 0;
            if (vm_mmap(task, frame->rdi, (size_t)frame->rsi,
                        (int)frame->rdx, flags, file, frame->r9, &mapped) != 0) {
                return (uint64_t)-1;
            }
            return mapped;
        }

        case SYS_MUNMAP:
            if (!task || vm_munmap(task, frame->rdi, (size_t)frame->rsi) != 0) {
                return (uint64_t)-1;
            }
            return 0;

        case SYS_PIPE:
            return syscall_pipe(task, frame->rdi) == 0 ? 0 : (uint64_t)-1;

        case SYS_GETTIMEOFDAY:
            return syscall_gettimeofday(frame->rdi) == 0 ? 0 : (uint64_t)-1;

        case SYS_MKDIR: {
            char path[USER_PATH_MAX];
            if (!task ||
                read_user_string(path, (const void*)(uintptr_t)frame->rdi,
                                 sizeof(path)) != 0 ||
                vfs_mkdir_at(path, task->cwd) != 0) return (uint64_t)-1;
            return 0;
        }

        case SYS_CHDIR: {
            char path[USER_PATH_MAX];
            char new_cwd[VFS_PATH_MAX];
            if (!task ||
                read_user_string(path, (const void*)(uintptr_t)frame->rdi,
                                 sizeof(path)) != 0 ||
                vfs_chdir(path, task->cwd, new_cwd, sizeof(new_cwd)) != 0) {
                return (uint64_t)-1;
            }
            for (size_t i = 0; i < sizeof(task->cwd); i++) {
                task->cwd[i] = new_cwd[i];
                if (new_cwd[i] == '\0') break;
            }
            return 0;
        }

        case SYS_GETCWD: {
            if (!task || frame->rdi == 0 || frame->rsi == 0) return (uint64_t)-1;
            size_t length = 0;
            while (length < sizeof(task->cwd) && task->cwd[length] != '\0') length++;
            if (length == sizeof(task->cwd) || frame->rsi < length + 1 ||
                validate_user_range(frame->rdi, length + 1, 1) != 0 ||
                copy_user_memory(task->cwd, frame->rdi, length + 1, 1) != 0) {
                return (uint64_t)-1;
            }
            return frame->rdi;
        }

        case SYS_KILL:
            if (frame->rdi > INT64_MAX || frame->rsi > UINT32_MAX ||
                process_send_signal((int64_t)frame->rdi,
                                    (uint32_t)frame->rsi) != 0) {
                return (uint64_t)-1;
            }
            return 0;

        default:
            return (uint64_t)-1;
    }
}
