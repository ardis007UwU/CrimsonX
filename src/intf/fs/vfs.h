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
#define VFS_FD_MAX    32
#define VFS_FD_STDIN  0
#define VFS_FD_STDOUT 1
#define VFS_FD_STDERR 2
#define VFS_ACCESS_READ  1
#define VFS_ACCESS_WRITE 2
#define VFS_SEEK_SET 0
#define VFS_SEEK_CUR 1
#define VFS_SEEK_END 2
#define VFS_PATH_MAX 256

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
    /** Resolve a child name to a fresh node owned by the caller. */
    vfs_node_t* (*lookup)(vfs_node_t* dir, const char* name);
    /** Remove a child entry from this directory. */
    int (*unlink)(vfs_node_t* dir, const char* name);
    /** Create a child directory within this directory. */
    int (*mkdir)(vfs_node_t* dir, const char* name);
    /** Open per-node resources; return zero on success. */
    int (*open)(vfs_node_t* node);
    /** Release per-node resources before the node is freed. */
    int (*close)(vfs_node_t* node);
    /** Read up to count bytes, returning bytes moved or -1 on error. */
    int (*read)(vfs_node_t* node, void* buf, size_t count);
    /** Write up to count bytes, returning bytes moved or -1 on error. */
    int (*write)(vfs_node_t* node, const void* buf, size_t count);
    /** Set the node cursor; whence values are VFS_SEEK_* constants. */
    int (*seek)(vfs_node_t* node, int64_t offset, int whence, uint64_t* new_offset);
};

typedef struct {
    vfs_node_t* node;
    size_t offset;
    int owns_node;   /**< Free node via close when the descriptor is released. */
    uint8_t access_mode;
    uint32_t references;
} vfs_file_t;

/** Initialize VFS state before mounting a filesystem. */
void vfs_init(void);

/**
 * Open a path relative to the filesystem root.
 * @param path NUL-terminated path with slash-separated components.
 * @return Fresh open node, owned by the caller, or NULL on failure.
 */
vfs_node_t* vfs_open(const char* path);

/** Open a path relative to cwd, resolving "." and ".." components. */
vfs_node_t* vfs_open_at(const char* path, const char* cwd);
/** Normalize a path to an absolute, canonical path. */
int vfs_resolve_path(const char* path, const char* cwd, char* output, size_t capacity);
/** Verify a path names a directory and return its canonical absolute path. */
int vfs_chdir(const char* path, const char* cwd, char* output, size_t capacity);
/** Create a directory at a path relative to cwd. */
int vfs_mkdir_at(const char* path, const char* cwd);
/**
 * Close and release a node returned by vfs_open().
 * @param node Node to close.
 * @return 0 on success, -1 for an invalid node.
 */
int vfs_close(vfs_node_t* node);

/** @return The shared console output channel. */
vfs_node_t* vfs_console_channel(void);
/** @return The shared keyboard input channel. */
vfs_node_t* vfs_keyboard_channel(void);

/** Install the mounted root directory node. */
void vfs_set_root(vfs_node_t* root);
/** @return The mounted root directory, or NULL if none is mounted. */
vfs_node_t* vfs_get_root(void);

/**
 * Remove an entry at a path relative to the filesystem root.
 * @return Filesystem result, or -1 for invalid paths/unsupported operations.
 */
int vfs_unlink(const char* path);
/** Remove an entry at a path relative to cwd. */
int vfs_unlink_at(const char* path, const char* cwd);

/**
 * Read up to count bytes from an open file.
 * @return Bytes read, zero at end of stream, or -1 on error.
 */
int vfs_file_read(vfs_file_t* file, void* buf, size_t count);
/**
 * Write up to count bytes to an open file.
 * @return Bytes written, or -1 on error.
 */
int vfs_file_write(vfs_file_t* file, const void* buf, size_t count);
/**
 * Set a file's cursor relative to SEEK_SET (0), SEEK_CUR (1), or SEEK_END (2).
 * @return 0 on success, -1 for unsupported seeks or invalid offsets.
 */
int vfs_file_seek(vfs_file_t* file, int64_t offset, int whence);
/** Add one descriptor reference to a shared open-file instance. */
int vfs_file_retain(vfs_file_t* file);
/** Release one reference and close/free the open-file instance at zero. */
int vfs_file_release(vfs_file_t* file);
/** Create a blocking in-memory pipe and return its read/write endpoint nodes. */
int vfs_pipe_create(vfs_node_t** read_node, vfs_node_t** write_node);

#endif
