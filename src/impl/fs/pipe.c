#include "fs/vfs.h"
#include "kernel/process.h"
#include "mm/kmalloc.h"

#define PIPE_CAPACITY 4096

typedef struct {
    uint8_t bytes[PIPE_CAPACITY];
    size_t read_at;
    size_t write_at;
    size_t used;
    uint32_t readers;
    uint32_t writers;
} pipe_state_t;

typedef struct {
    pipe_state_t* state;
    int is_reader;
} pipe_endpoint_t;

static int pipe_read(vfs_node_t* node, void* buffer, size_t count) {
    if (!node || !node->data || (count && !buffer)) return -1;
    pipe_endpoint_t* endpoint = (pipe_endpoint_t*)node->data;
    if (!endpoint->is_reader) return -1;
    pipe_state_t* state = endpoint->state;
    if (!state) return -1;
    if (count == 0) return 0;

    while (state->used == 0 && state->writers != 0) process_block_on(state);
    if (state->used == 0) return 0;

    size_t moved = count < state->used ? count : state->used;
    uint8_t* output = (uint8_t*)buffer;
    for (size_t i = 0; i < moved; i++) {
        output[i] = state->bytes[state->read_at];
        state->read_at = (state->read_at + 1) % PIPE_CAPACITY;
    }
    state->used -= moved;
    process_wakeup_channel(state);
    return (int)moved;
}

static int pipe_write(vfs_node_t* node, const void* buffer, size_t count) {
    if (!node || !node->data || (count && !buffer)) return -1;
    pipe_endpoint_t* endpoint = (pipe_endpoint_t*)node->data;
    if (endpoint->is_reader) return -1;
    pipe_state_t* state = endpoint->state;
    if (!state) return -1;
    if (count == 0) return 0;

    const uint8_t* input = (const uint8_t*)buffer;
    size_t moved = 0;
    while (moved < count) {
        if (state->readers == 0) return moved ? (int)moved : -1;
        if (state->used == PIPE_CAPACITY) {
            process_block_on(state);
            continue;
        }
        size_t available = PIPE_CAPACITY - state->used;
        size_t chunk = count - moved;
        if (chunk > available) chunk = available;
        for (size_t i = 0; i < chunk; i++) {
            state->bytes[state->write_at] = input[moved + i];
            state->write_at = (state->write_at + 1) % PIPE_CAPACITY;
        }
        state->used += chunk;
        moved += chunk;
        process_wakeup_channel(state);
    }
    return (int)moved;
}

static int pipe_close(vfs_node_t* node) {
    if (!node || !node->data) return -1;
    pipe_endpoint_t* endpoint = (pipe_endpoint_t*)node->data;
    pipe_state_t* state = endpoint->state;
    if (!state) {
        kfree(endpoint);
        node->data = NULL;
        return -1;
    }

    if (endpoint->is_reader) {
        if (state->readers) state->readers--;
    } else if (state->writers) {
        state->writers--;
    }
    process_wakeup_channel(state);
    kfree(endpoint);
    node->data = NULL;
    if (state->readers == 0 && state->writers == 0) kfree(state);
    return 0;
}

static vfs_node_t* pipe_node_create(const char* name, pipe_state_t* state,
                                    int is_reader) {
    vfs_node_t* node = (vfs_node_t*)kmalloc(sizeof(*node));
    pipe_endpoint_t* endpoint = (pipe_endpoint_t*)kmalloc(sizeof(*endpoint));
    if (!node || !endpoint) {
        kfree(node);
        kfree(endpoint);
        return NULL;
    }
    endpoint->state = state;
    endpoint->is_reader = is_reader;
    node->name = name;
    node->type = VFS_TYPE_CHANNEL;
    node->size = 0;
    node->data = endpoint;
    node->lookup = NULL;
    node->unlink = NULL;
    node->mkdir = NULL;
    node->open = NULL;
    node->close = pipe_close;
    node->read = is_reader ? pipe_read : NULL;
    node->write = is_reader ? NULL : pipe_write;
    node->seek = NULL;
    return node;
}

int vfs_pipe_create(vfs_node_t** read_node, vfs_node_t** write_node) {
    if (!read_node || !write_node) return -1;
    *read_node = NULL;
    *write_node = NULL;

    pipe_state_t* state = (pipe_state_t*)kmalloc(sizeof(*state));
    if (!state) return -1;
    state->read_at = 0;
    state->write_at = 0;
    state->used = 0;
    state->readers = 1;
    state->writers = 1;

    vfs_node_t* reader = pipe_node_create("pipe-read", state, 1);
    if (!reader) {
        kfree(state);
        return -1;
    }
    vfs_node_t* writer = pipe_node_create("pipe-write", state, 0);
    if (!writer) {
        pipe_close(reader);
        kfree(reader);
        return -1;
    }
    *read_node = reader;
    *write_node = writer;
    return 0;
}
