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
    if (count && !buf) return -1;
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
    return vfs_unlink_at(path, "/");
}

void vfs_init(void) {
    vfs_root = NULL;
}

static size_t bounded_strlen(const char* value, size_t limit) {
    size_t length = 0;
    if (!value) return limit;
    while (length < limit && value[length] != '\0') length++;
    return length;
}

static int append_path_components(const char* source, size_t source_length,
                                  char* output, size_t capacity, size_t* length) {
    size_t position = 0;
    while (position < source_length) {
        while (position < source_length && source[position] == '/') position++;
        if (position == source_length) break;

        size_t begin = position;
        while (position < source_length && source[position] != '/') position++;
        size_t component_length = position - begin;
        if (component_length == 1 && source[begin] == '.') continue;
        if (component_length == 2 && source[begin] == '.' &&
            source[begin + 1] == '.') {
            if (*length > 1) {
                size_t trim = *length;
                while (trim > 1 && output[trim - 1] != '/') trim--;
                *length = trim > 1 ? trim - 1 : 1;
                output[*length] = '\0';
            }
            continue;
        }
        if (component_length > 55) return -1;

        size_t separator = *length > 1 ? 1 : 0;
        if (*length >= capacity || separator > capacity - *length ||
            component_length >= capacity - *length - separator) return -1;
        if (separator) output[(*length)++] = '/';
        for (size_t i = 0; i < component_length; i++) {
            output[(*length)++] = source[begin + i];
        }
        output[*length] = '\0';
    }
    return 0;
}

int vfs_resolve_path(const char* path, const char* cwd, char* output,
                     size_t capacity) {
    if (!path || !cwd || !output || capacity < 2) return -1;
    size_t path_length = bounded_strlen(path, VFS_PATH_MAX);
    size_t cwd_length = bounded_strlen(cwd, VFS_PATH_MAX);
    if (path_length >= VFS_PATH_MAX || cwd_length >= VFS_PATH_MAX ||
        cwd_length == 0 || cwd[0] != '/') return -1;

    output[0] = '/';
    output[1] = '\0';
    size_t length = 1;
    if (path[0] != '/' &&
        append_path_components(cwd, cwd_length, output, capacity, &length) != 0) {
        return -1;
    }
    if (append_path_components(path, path_length, output, capacity, &length) != 0) {
        return -1;
    }
    return 0;
}

vfs_node_t* vfs_open_at(const char* path, const char* cwd) {
    if (!vfs_root || !vfs_root->lookup) return NULL;
    char normalized[VFS_PATH_MAX];
    if (vfs_resolve_path(path, cwd, normalized, sizeof(normalized)) != 0) return NULL;

    vfs_node_t* current = vfs_root->lookup(vfs_root, "");
    if (!current || current->type != VFS_TYPE_DIR) {
        if (current) vfs_close(current);
        return NULL;
    }

    size_t position = 1;
    size_t path_length = strlen(normalized);
    while (position < path_length) {
        size_t begin = position;
        while (position < path_length && normalized[position] != '/') position++;
        size_t component_length = position - begin;
        if (component_length == 0 || component_length > 55 ||
            current->type != VFS_TYPE_DIR || !current->lookup) {
            vfs_close(current);
            return NULL;
        }

        char component[56];
        for (size_t i = 0; i < component_length; i++) {
            component[i] = normalized[begin + i];
        }
        component[component_length] = '\0';

        vfs_node_t* child = current->lookup(current, component);
        vfs_close(current);
        if (!child) return NULL;
        current = child;
        if (position < path_length) position++;
    }

    if (current->open && current->open(current) != 0) {
        vfs_close(current);
        return NULL;
    }
    return current;
}

vfs_node_t* vfs_open(const char* path) {
    return vfs_open_at(path, "/");
}

int vfs_chdir(const char* path, const char* cwd, char* output, size_t capacity) {
    if (vfs_resolve_path(path, cwd, output, capacity) != 0) return -1;
    vfs_node_t* node = vfs_open_at(output, "/");
    if (!node) return -1;
    int is_directory = node->type == VFS_TYPE_DIR;
    vfs_close(node);
    return is_directory ? 0 : -1;
}

static int split_parent_path(const char* path, const char* cwd,
                             char* parent, size_t parent_capacity,
                             char* leaf, size_t leaf_capacity) {
    char normalized[VFS_PATH_MAX];
    if (vfs_resolve_path(path, cwd, normalized, sizeof(normalized)) != 0) return -1;
    size_t length = strlen(normalized);
    if (length <= 1) return -1;

    size_t separator = length;
    while (separator > 0 && normalized[separator - 1] != '/') separator--;
    if (separator == 0) return -1;
    size_t leaf_length = length - separator;
    if (leaf_length == 0 || leaf_length >= leaf_capacity ||
        separator >= parent_capacity) return -1;

    size_t parent_length = separator == 1 ? 1 : separator - 1;
    for (size_t i = 0; i < parent_length; i++) parent[i] = normalized[i];
    parent[parent_length] = '\0';
    for (size_t i = 0; i < leaf_length; i++) leaf[i] = normalized[separator + i];
    leaf[leaf_length] = '\0';
    return 0;
}

int vfs_mkdir_at(const char* path, const char* cwd) {
    if (!vfs_root) return -1;
    char parent_path[VFS_PATH_MAX];
    char leaf[56];
    if (split_parent_path(path, cwd, parent_path, sizeof(parent_path),
                          leaf, sizeof(leaf)) != 0) return -1;
    vfs_node_t* parent = vfs_open_at(parent_path, "/");
    if (!parent) return -1;
    int result = (parent->type == VFS_TYPE_DIR && parent->mkdir)
                     ? parent->mkdir(parent, leaf) : -1;
    vfs_close(parent);
    return result;
}

int vfs_unlink_at(const char* path, const char* cwd) {
    if (!vfs_root) return -1;
    char parent_path[VFS_PATH_MAX];
    char leaf[56];
    if (split_parent_path(path, cwd, parent_path, sizeof(parent_path),
                          leaf, sizeof(leaf)) != 0) return -1;
    vfs_node_t* parent = vfs_open_at(parent_path, "/");
    if (!parent) return -1;
    int result = (parent->type == VFS_TYPE_DIR && parent->unlink)
                     ? parent->unlink(parent, leaf) : -1;
    vfs_close(parent);
    return result;
}

int vfs_close(vfs_node_t* node) {
    if (!node) return -1;
    int result = node->close ? node->close(node) : 0;
    if (node != &console_node && node != &keyboard_node) kfree(node);
    return result;
}

int vfs_file_read(vfs_file_t* file, void* buf, size_t count) {
    if (!file || !file->node || !file->node->read ||
        !(file->access_mode & VFS_ACCESS_READ) || (count && !buf) ||
        count > 0x7fffffffU) return -1;
    int n = file->node->read(file->node, buf, count);
    if (n > 0) file->offset += (size_t)n;
    return n;
}

int vfs_file_write(vfs_file_t* file, const void* buf, size_t count) {
    if (!file || !file->node || !file->node->write ||
        !(file->access_mode & VFS_ACCESS_WRITE) || (count && !buf) ||
        count > 0x7fffffffU) return -1;
    int n = file->node->write(file->node, buf, count);
    if (n > 0) file->offset += (size_t)n;
    return n;
}

int vfs_file_seek(vfs_file_t* file, int64_t offset, int whence) {
    if (!file || !file->node || !file->node->seek) return -1;
    uint64_t new_offset = 0;
    if (file->node->seek(file->node, offset, whence, &new_offset) != 0 ||
        new_offset > (uint64_t)(size_t)-1) return -1;
    file->offset = (size_t)new_offset;
    return 0;
}

int vfs_file_retain(vfs_file_t* file) {
    if (!file || file->references == 0 || file->references == UINT32_MAX) return -1;
    file->references++;
    return 0;
}

int vfs_file_release(vfs_file_t* file) {
    if (!file || file->references == 0) return -1;
    file->references--;
    if (file->references != 0) return 0;

    int result = 0;
    if (file->owns_node && file->node) result = vfs_close(file->node);
    kfree(file);
    return result;
}
