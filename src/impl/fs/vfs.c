#include "fs/vfs.h"
#include "print.h"
#include "keyboard.h"
#include "mm/kmalloc.h"
#include "string.h"

static vfs_node_t* vfs_root = NULL;

/* ---------------------------------------------------------------------- */
/* Console channel: writes stream to the VGA terminal (plus serial mirror) */
/* ---------------------------------------------------------------------- */

static int console_open(vfs_node_t* node) {
    (void)node;
    return 0;
}

static int console_close(vfs_node_t* node) {
    (void)node;
    return 0;
}

static int console_write(vfs_node_t* node, const void* buf, size_t count) {
    (void)node;
    const char* s = (const char*)buf;
    for (size_t i = 0; i < count; i++) {
        char c = s[i];
        if (c != '\0') print_char(c);
    }
    return (int)count;
}

static int console_read(vfs_node_t* node, void* buf, size_t count) {
    (void)node; (void)buf; (void)count;
    return -1;   /* console is write-only */
}

/* ---------------------------------------------------------------------- */
/* Keyboard channel: read pops the ASCII ring buffer. Non-blocking here;   */
/* the syscall layer provides blocking behavior for SYS_READ.              */
/* ---------------------------------------------------------------------- */

static int keyboard_open(vfs_node_t* node) {
    (void)node;
    return 0;
}

static int keyboard_close(vfs_node_t* node) {
    (void)node;
    return 0;
}

static int keyboard_read(vfs_node_t* node, void* buf, size_t count) {
    (void)node;
    return keyboard_pop_bytes((char*)buf, count);
}

static int keyboard_write(vfs_node_t* node, const void* buf, size_t count) {
    (void)node; (void)buf; (void)count;
    return -1;   /* stdin devices are read-only */
}

/* ---------------------------------------------------------------------- */

static vfs_node_t console_node = {
    .name = "console",
    .type = VFS_TYPE_CHANNEL,
    .size = 0,
    .data = 0,
    .open = console_open,
    .close = console_close,
    .read = console_read,
    .write = console_write,
};

static vfs_node_t keyboard_node = {
    .name = "keyboard",
    .type = VFS_TYPE_CHANNEL,
    .size = 0,
    .data = 0,
    .open = keyboard_open,
    .close = keyboard_close,
    .read = keyboard_read,
    .write = keyboard_write,
};

vfs_node_t* vfs_console_channel(void) { return &console_node; }
vfs_node_t* vfs_keyboard_channel(void) { return &keyboard_node; }

void vfs_set_root(vfs_node_t* root) {
    vfs_root = root;
}

vfs_node_t* vfs_get_root(void) {
    return vfs_root;
}

int vfs_unlink(const char* path) {
    if (!vfs_root || !vfs_root->unlink || !path || path[0] == '\0') return -1;

    const char* p = path;
    if (p[0] == '/') p++;
    if (p[0] == '\0') return -1;

    size_t len = 0;
    while (p[len] != '\0' && p[len] != '/') len++;
    if (p[len] != '\0') return -1;

    char leaf[64];
    if (len >= sizeof(leaf)) return -1;
    for (size_t i = 0; i < len; i++) leaf[i] = p[i];
    leaf[len] = '\0';

    return vfs_root->unlink(vfs_root, leaf);
}

void vfs_init(void) {
    vfs_root = NULL;
}

vfs_node_t* vfs_open(const char* path) {
    if (!vfs_root || !vfs_root->lookup) return NULL;

    /* "/" (or "") resolves through the fs driver so fresh node state is
     * owned per-open; the fs special-cases the empty name for its root. */
    const char* p = path;
    if (p[0] == '/') p++;
    size_t len = 0;
    while (p[len] != '\0' && p[len] != '/') len++;

    if (len == 0) {
        return vfs_root->lookup(vfs_root, "");
    }

    /* Single-level name resolution (one path component). */
    char leaf[64];
    if (len >= sizeof(leaf)) return NULL;
    for (size_t i = 0; i < len; i++) leaf[i] = p[i];
    leaf[len] = '\0';

    vfs_node_t* found = vfs_root->lookup(vfs_root, leaf);
    if (!found) return NULL;
    return found;
}

int vfs_close(vfs_node_t* node) {
    if (!node) return -1;
    if (node->close) node->close(node);
    kfree(node);
    return 0;
}

int vfs_file_read(vfs_file_t* file, void* buf, size_t count) {
    if (!file || !file->node || !file->node->read) return -1;
    int n = file->node->read(file->node, buf, count);
    if (n > 0) file->offset += (size_t)n;
    return n;
}

int vfs_file_write(vfs_file_t* file, const void* buf, size_t count) {
    if (!file || !file->node || !file->node->write) return -1;
    int n = file->node->write(file->node, buf, count);
    if (n > 0) file->offset += (size_t)n;
    return n;
}
