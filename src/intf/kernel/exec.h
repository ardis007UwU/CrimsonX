#ifndef EXEC_H
#define EXEC_H

#include "kernel/process.h"

/*
 * execve machinery.
 *
 * exec_replace_current() is the single entry point used by both the
 * SYS_EXECVE syscall (ring-3 task swapping its image) and the spawn wrapper
 * (shell-created kernel task bootstrapping into user mode). On success it
 * tears the current address space down, flips into the freshly built user
 * image via the exec_drop stub and NEVER returns. Returns -1 on failure
 * with the calling task untouched.
 */
int exec_replace_current(task_t* task, const char* path);

/* Spawn wrapper helpers (called by the shell instead of the syscall). */
task_t* process_spawn_user(const char* path);

#endif
