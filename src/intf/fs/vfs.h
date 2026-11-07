#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>

/*
 * Virtual filesystem.
 *
 * vfs_node_t is the vnode: every node carries the function pointers its kind
 * supports (open/read/write/close). Device nodes (console, keyboard) are
 * baked into the kernel; filesystem nodes are created by the filesystem
 * driver's mount and handed out by vfs_open().
 *
 * vfs_file_t is the per-task "open instance": a node plus its private file
 * cursor. task_t holds a table of these; FD 0 = stdin, 1 = stdout, 2 = stderr.
 */
#define VFS_FD_MAX    16
#define VFS_FD_STDIN  0
#define VFS_FD_STDOUT 1
#define VFS_FD_STDERR 2

#define VFS_TYPE_UNKNOWN 0
#define VFS_TYPE_FILE    1
#define VFS_TYPE_DIR     2
#define VFS_TYPE_CHANNEL 3   /* character device (console, keyboard) */

typedef struct vfs_node vfs_node_t;

struct vfs_node {
    const char* name;
    uint32_t type;
    size_t size;          /* file size in bytes; channels report last op size */
    void* data;           /* fs/impl-private context, freed by close() */
    /* Directory lookup (dir nodes): resolve `name` inside `dir`. Returns a
     * fresh open node (fs-owned copy, freed via close()) or NULL. */
    vfs_node_t* (*lookup)(vfs_node_t* dir, const char* name);
    int (*unlink)(vfs_node_t* dir, const char* name);
    int (*open)(vfs_node_t* node);
    int (*close)(vfs_node_t* node);
    /* Byte-stream I/O; files keep their cursor inside node->data.
     * Return the number of bytes moved (>= 0) or -1 on error. */
    int (*read)(vfs_node_t* node, void* buf, size_t count);
    int (*write)(vfs_node_t* node, const void* buf, size_t count);
};

typedef struct {
    vfs_node_t* node;
    size_t offset;
    int owns_node;   /* free node via close when the descriptor dies */
} vfs_file_t;

void vfs_init(void);

/* Open `path` relative to the root. Returns a fresh node copy (caller frees
 * with vfs_close(node)) or NULL. */
vfs_node_t* vfs_open(const char* path);

/* fs-provided way to close the node (vfs_close frees) */
int vfs_close(vfs_node_t* node);

/* Built-in channels for stdin/stdout/stderr defaults. */
vfs_node_t* vfs_console_channel(void);
vfs_node_t* vfs_keyboard_channel(void);

/* Install the root directory produced by a filesystem driver. */
void vfs_set_root(vfs_node_t* root);
vfs_node_t* vfs_get_root(void);

/* Remove a single file or directory entry relative to the mounted root. */
int vfs_unlink(const char* path);

/* Consume up to `count` bytes from an open file descriptor object. */
int vfs_file_read(vfs_file_t* file, void* buf, size_t count);
int vfs_file_write(vfs_file_t* file, const void* buf, size_t count);

#endif
