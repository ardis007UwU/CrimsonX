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
/**
 * Replace a task's image with a VFS-backed executable.
 * @return -1 on failure; does not return after a successful image switch.
 */
int exec_replace_current(task_t* task, const char* path);
/** Replace a task image while supplying copied argument and environment vectors. */
int exec_replace_current_args(task_t* task, const char* path,
                              const char* const argv[], size_t argc,
                              const char* const envp[], size_t envc);
/**
 * Replace an image with copied arguments and release owned_args once loading
 * has succeeded; the caller retains ownership if the call returns an error.
 */
int exec_replace_current_args_owned(task_t* task, const char* path,
                                    const char* const argv[], size_t argc,
                                    const char* const envp[], size_t envc,
                                    void** owned_args);

/** Create a task that attempts to execute path in user mode. */
task_t* process_spawn_user(const char* path);

#endif
