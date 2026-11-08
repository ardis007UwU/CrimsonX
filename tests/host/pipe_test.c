#include "fs/vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;
static int failures;
static int allocations;
static int releases;

#define CHECK(expression) do { \
    checks++; \
    if (!(expression)) { \
        failures++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
    } \
} while (0)

void* kmalloc(size_t size) {
    void* memory = malloc(size);
    if (memory) allocations++;
    return memory;
}

void kfree(void* memory) {
    if (!memory) return;
    releases++;
    free(memory);
}

void process_block_on(const void* channel) {
    (void)channel;
}

void process_wakeup_channel(const void* channel) {
    (void)channel;
}

int vfs_close(vfs_node_t* node) {
    if (!node) return -1;
    int result = node->close ? node->close(node) : 0;
    kfree(node);
    return result;
}

static void test_buffer_wrap_and_eof(void) {
    vfs_node_t* reader = NULL;
    vfs_node_t* writer = NULL;
    CHECK(vfs_pipe_create(&reader, &writer) == 0);
    CHECK(reader != NULL && writer != NULL);

    unsigned char first[4090];
    unsigned char consumed[4080];
    unsigned char tail[30];
    unsigned char expected[30];
    unsigned char next[20];
    memset(first, 'a', sizeof(first));
    memset(next, 'b', sizeof(next));
    memset(expected, 'a', 10);
    memset(expected + 10, 'b', 20);

    CHECK(writer->write(writer, first, sizeof(first)) == (int)sizeof(first));
    CHECK(reader->read(reader, consumed, sizeof(consumed)) == (int)sizeof(consumed));
    CHECK(writer->write(writer, next, sizeof(next)) == (int)sizeof(next));
    CHECK(reader->read(reader, tail, sizeof(tail)) == (int)sizeof(tail));
    CHECK(memcmp(tail, expected, sizeof(tail)) == 0);

    CHECK(vfs_close(writer) == 0);
    CHECK(reader->read(reader, tail, sizeof(tail)) == 0);
    CHECK(vfs_close(reader) == 0);
}

static void test_closed_reader_rejects_writes(void) {
    vfs_node_t* reader = NULL;
    vfs_node_t* writer = NULL;
    CHECK(vfs_pipe_create(&reader, &writer) == 0);
    CHECK(vfs_close(reader) == 0);
    CHECK(writer->write(writer, "x", 1) == -1);
    CHECK(vfs_close(writer) == 0);
}

int main(void) {
    test_buffer_wrap_and_eof();
    test_closed_reader_rejects_writes();
    CHECK(allocations == releases);
    printf("pipe_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
