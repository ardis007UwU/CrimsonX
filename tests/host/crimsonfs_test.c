/*
 * Host-side tests for src/impl/fs/crimsonfs.c + the vfs_open wrapper
 * (run with: make test-host)
 *
 * The ATA layer is replaced by an in-memory blkdev_t, so the whole driver
 * stack (format / mount / create / read / write) runs under ASan + UBSan.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "fs/crimsonfs.h"
#include "fs/vfs.h"
#include "drivers/rtc.h"
#include "mm/kmalloc.h"
#include "string.h"   /* src/intf/string.h via -I; system one is shadowed */

/* ================================================================== */
/* Stubs for kernel services the driver normally calls                */
/* ================================================================== */

void* kmalloc(size_t size) { return malloc(size); }
void kfree(void* p) { free(p); }

static uint64_t fake_unix_time = 1700000000ULL;
int rtc_get_unix_time(uint64_t* seconds) {
    if (!seconds) return -1;
    *seconds = fake_unix_time;
    return 0;
}

static void stub_print(void) { /* quiet */ }

void print_set_color(uint8_t f, uint8_t b) { (void)f; (void)b; stub_print(); }
void print_str(char* s) { (void)s; stub_print(); }
void print_char(char c) { (void)c; stub_print(); }
void print_uint64_dec(uint64_t n) { (void)n; stub_print(); }
void print_uint64_hex(uint64_t n) { (void)n; stub_print(); }

int keyboard_pop_bytes(char* buf, size_t count) { (void)buf; (void)count; return 0; }

/* ================================================================== */
/* Fake block device: plain array backing store                       */
/* ================================================================== */

#define FAKE_BLOCKS 2048   /* 1 MiB */

static uint8_t backing[FAKE_BLOCKS][BLKDEV_BLOCK_SIZE];
static long long fake_reads = 0, fake_writes = 0;

static int fake_read(blkdev_t* dev, uint64_t lba, uint32_t count, void* buf) {
    (void)dev;
    if (lba + count > FAKE_BLOCKS) return -1;
    memcpy(buf, backing[lba], (size_t)count * BLKDEV_BLOCK_SIZE);
    fake_reads++;
    return 0;
}

static int fake_write(blkdev_t* dev, uint64_t lba, uint32_t count, const void* buf) {
    (void)dev;
    if (lba + count > FAKE_BLOCKS) return -1;
    memcpy(backing[lba], buf, (size_t)count * BLKDEV_BLOCK_SIZE);
    fake_writes++;
    return 0;
}

static blkdev_t fake_dev = {
    .name = "fake0",
    .block_count = FAKE_BLOCKS,
    .read_blocks = fake_read,
    .write_blocks = fake_write,
};

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { failures++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    else { printf("PASS %s\n", #cond); } \
} while (0)

#define PAYLOAD_LEN 2000   /* ~4 blocks: exercises multi-block splicing */

static void test_format_and_empty_mount(void) {
    CHECK(crimsonfs_format(&fake_dev) == 0);

    /* superblock on disk */
    crimsonfs_super_t* sb = (crimsonfs_super_t*)backing[0];
    CHECK(strcmp(sb->magic, "CRIMFS0") == 0);
    CHECK(sb->version == CRIMSONFS_VERSION);
    CHECK(sb->total_blocks == FAKE_BLOCKS);
    CHECK(sb->free_blocks == FAKE_BLOCKS - CRIMSONFS_DATA_LBA);

    vfs_node_t* root = crimsonfs_mount(&fake_dev);
    CHECK(root != NULL);
    if (root) {
        CHECK(root->type == VFS_TYPE_DIR);
        CHECK(root->size == 0);            /* empty root directory */
    }

    /* mount is single: second mount refuses */
    CHECK(crimsonfs_mount(&fake_dev) == NULL);
    CHECK(crimsonfs_unmount() == 0);
    CHECK(crimsonfs_unmount() != 0);
}

static void test_create_write_read(void) {
    CHECK(crimsonfs_format(&fake_dev) == 0);
    vfs_node_t* root = crimsonfs_mount(&fake_dev);
    CHECK(root != NULL);
    if (!root) return;

    CHECK(vfs_get_root() == root);

    /* names longer than the dirent field are refused */
    char longname[80];
    for (int i = 0; i < 79; i++) longname[i] = 'a';
    longname[79] = '\0';
    CHECK(crimsonfs_create(root, longname) != 0);

    CHECK(crimsonfs_create(root, "prog") == 0);
    CHECK(crimsonfs_create(root, "prog") != 0);      /* duplicate rejected */
    CHECK(crimsonfs_create(root, "notes") == 0);

    /* find it through the VFS and splice a multi-block payload */
    vfs_node_t* n = vfs_open("/prog");
    CHECK(n != NULL);
    if (n) {
        CHECK(n->type == VFS_TYPE_FILE);
        uint8_t* out = malloc(PAYLOAD_LEN);
        for (int i = 0; i < PAYLOAD_LEN; i++) out[i] = (uint8_t)(i * 13 + 1);

        CHECK(n->write(n, out, PAYLOAD_LEN) == PAYLOAD_LEN);
        CHECK(n->size == PAYLOAD_LEN);
        vfs_close(n);

        n = vfs_open("/prog");
        CHECK(n != NULL);

        uint8_t* back = calloc(1, PAYLOAD_LEN);
        int got = 0;
        while (got < PAYLOAD_LEN) {
            int r = n->read(n, back + got, PAYLOAD_LEN - got);
            if (r <= 0) break;
            got += r;
        }
        CHECK(got == PAYLOAD_LEN);
        CHECK(memcmp(back, out, PAYLOAD_LEN) == 0);

        CHECK(n->read(n, back, 4) == 0);   /* EOF */
        free(out);
        free(back);
        vfs_close(n);
    }

    /* entries visible through a fresh dir handle */
    vfs_node_t* dir = vfs_open("/");
    CHECK(dir != NULL);
    if (dir) {
        uint8_t block[512];
        int seen_prog = 0, seen_notes = 0;
        for (;;) {
            int got = dir->read(dir, block, sizeof(block));
            if (got <= 0) break;
            for (size_t off = 0; off + sizeof(crimsonfs_dirent_t) <= (size_t)got;
                 off += sizeof(crimsonfs_dirent_t)) {
                crimsonfs_dirent_t* e = (crimsonfs_dirent_t*)(block + off);
                if (e->inode == 0) continue;
                if (strcmp(e->name, "prog") == 0) seen_prog++;
                if (strcmp(e->name, "notes") == 0) seen_notes++;
            }
            if (got < (int)sizeof(block)) break;
        }
        CHECK(seen_prog == 1);
        CHECK(seen_notes == 1);
        vfs_close(dir);
    }

    crimsonfs_unmount();
}

static void test_persistence_across_mounts(void) {
    CHECK(crimsonfs_format(&fake_dev) == 0);
    vfs_node_t* root = crimsonfs_mount(&fake_dev);
    CHECK(crimsonfs_create(root, "keep") == 0);

    uint8_t payload[300];
    for (int i = 0; i < (int)sizeof(payload); i++) payload[i] = (uint8_t)(i + 111);
    vfs_node_t* n = vfs_open("/keep");
    CHECK(n != NULL);
    CHECK(n->write(n, payload, sizeof(payload)) == (int)sizeof(payload));
    vfs_close(n);
    crimsonfs_unmount();

    root = crimsonfs_mount(&fake_dev);
    CHECK(root != NULL);
    if (root) {
        CHECK(crimsonfs_create(root, "keep") != 0);   /* survived */
        n = vfs_open("/keep");
        CHECK(n != NULL);
        if (n) {
            uint8_t back[300];
            CHECK(n->read(n, back, sizeof(back)) == (int)sizeof(back));
            CHECK(memcmp(back, payload, sizeof(payload)) == 0);
            vfs_close(n);
        }
        crimsonfs_unmount();
    }

    /* a blank device simply fails to mount */
    memset(backing, 0, sizeof(backing));
    CHECK(crimsonfs_mount(&fake_dev) == NULL);
}

static void test_capacity_clamps(void) {
    CHECK(crimsonfs_format(&fake_dev) == 0);
    vfs_node_t* root = crimsonfs_mount(&fake_dev);
    if (!root) return;

    CHECK(crimsonfs_create(root, "big") == 0);
    vfs_node_t* n = vfs_open("/big");
    CHECK(n != NULL);
    if (n) {
        uint8_t buf[CRIMSONFS_MAX_FILE];
        memset(buf, 0x5A, sizeof(buf));
        CHECK(n->write(n, buf, sizeof(buf)) == (int)sizeof(buf));
        CHECK(n->size == sizeof(buf));

        uint8_t tail[256];
        memset(tail, 0xA5, sizeof(tail));
        CHECK(n->write(n, tail, sizeof(tail)) == 0);
        CHECK(n->size == sizeof(buf));
        vfs_close(n);
    }
    crimsonfs_unmount();
}

static void test_vfs_file_seek_and_access_modes(void) {
    CHECK(crimsonfs_format(&fake_dev) == 0);
    vfs_node_t* root = crimsonfs_mount(&fake_dev);
    CHECK(root != NULL);
    if (!root) return;
    CHECK(crimsonfs_create(root, "seek") == 0);

    vfs_file_t* file = calloc(1, sizeof(*file));
    CHECK(file != NULL);
    if (file) {
        file->node = vfs_open("/seek");
        file->owns_node = 1;
        file->access_mode = VFS_ACCESS_READ | VFS_ACCESS_WRITE;
        file->references = 1;
        CHECK(file->node != NULL);
    }
    if (file && file->node) {
        const char payload[] = "abcdefgh";
        char readback[sizeof(payload)] = {0};
        CHECK(vfs_file_write(file, payload, sizeof(payload) - 1) == (int)sizeof(payload) - 1);
        CHECK(file->offset == sizeof(payload) - 1);
        CHECK(vfs_file_seek(file, -4, 2) == 0);
        CHECK(file->offset == 4);
        CHECK(vfs_file_read(file, readback, 4) == 4);
        CHECK(memcmp(readback, "efgh", 4) == 0);
        CHECK(vfs_file_seek(file, 0, 0) == 0);

        file->access_mode = VFS_ACCESS_READ;
        CHECK(vfs_file_write(file, payload, 1) == -1);
        CHECK(vfs_file_read(file, readback, 1) == 1);
        CHECK(vfs_file_retain(file) == 0);
        CHECK(vfs_file_release(file) == 0);
        CHECK(vfs_file_release(file) == 0);
    } else if (file) {
        free(file);
    }

    CHECK(vfs_open(NULL) == NULL);
    CHECK(vfs_open("/seek/nested") == NULL);
    CHECK(crimsonfs_unmount() == 0);
}

static void test_unlink_removes_file_and_frees_blocks(void) {
    CHECK(crimsonfs_format(&fake_dev) == 0);
    vfs_node_t* root = crimsonfs_mount(&fake_dev);
    CHECK(root != NULL);
    if (!root) return;

    CHECK(crimsonfs_create(root, "drop") == 0);
    vfs_node_t* n = vfs_open("/drop");
    CHECK(n != NULL);
    uint32_t free_before_write = ((crimsonfs_super_t*)backing[0])->free_blocks;
    if (n) {
        uint8_t* payload = malloc(CRIMSONFS_MAX_FILE);
        CHECK(payload != NULL);
        if (!payload) {
            vfs_close(n);
            crimsonfs_unmount();
            return;
        }
        memset(payload, 0x7F, CRIMSONFS_MAX_FILE);
        CHECK(n->write(n, payload, CRIMSONFS_MAX_FILE) == (int)CRIMSONFS_MAX_FILE);
        vfs_close(n);
        free(payload);
    }

    CHECK(crimsonfs_unlink(root, "drop") == 0);
    CHECK(vfs_open("/drop") == NULL);
    CHECK(((crimsonfs_super_t*)backing[0])->free_blocks == free_before_write);
    crimsonfs_unmount();
}

static void test_creation_and_modification_timestamps(void) {
    fake_unix_time = 1700000000ULL;
    CHECK(crimsonfs_format(&fake_dev) == 0);
    vfs_node_t* root = crimsonfs_mount(&fake_dev);
    CHECK(root != NULL);
    if (!root) return;

    CHECK(crimsonfs_create(root, "dated") == 0);
    crimsonfs_inode_t* inodes = (crimsonfs_inode_t*)backing[CRIMSONFS_INODE_LBA];
    crimsonfs_dirent_t* entries =
        (crimsonfs_dirent_t*)&backing[inodes[1].direct[0]][0];
    uint32_t file_ino = 0;
    for (size_t i = 0; i < CRIMSONFS_BLOCK_SIZE / sizeof(*entries); i++) {
        if (entries[i].inode != 0 && strcmp(entries[i].name, "dated") == 0) {
            file_ino = entries[i].inode;
            break;
        }
    }
    CHECK(file_ino != 0);

    crimsonfs_timestamp_t* timestamps = (crimsonfs_timestamp_t*)
        ((uint8_t*)inodes + CRIMSONFS_TIMESTAMP_OFFSET);
    CHECK(timestamps[file_ino].creation_time == 1700000000U);
    CHECK(timestamps[file_ino].modification_time == 1700000000U);

    fake_unix_time += 125;
    vfs_node_t* file = vfs_open("/dated");
    CHECK(file != NULL);
    if (file) {
        CHECK(file->write(file, "x", 1) == 1);
        vfs_close(file);
    }
    CHECK(timestamps[file_ino].creation_time == 1700000000U);
    CHECK(timestamps[file_ino].modification_time == 1700000125U);
    CHECK(crimsonfs_unmount() == 0);
}

static void test_nested_directories_and_paths(void) {
    fake_unix_time = 1700000200ULL;
    CHECK(crimsonfs_format(&fake_dev) == 0);
    vfs_node_t* root = crimsonfs_mount(&fake_dev);
    CHECK(root != NULL);
    if (!root) return;

    CHECK(vfs_mkdir_at("a", "/") == 0);
    CHECK(vfs_mkdir_at("b", "/a") == 0);
    CHECK(vfs_mkdir_at("/a/b", "/") != 0);
    vfs_node_t* nested = vfs_open_at("/a/b", "/");
    CHECK(nested != NULL && nested->type == VFS_TYPE_DIR);
    if (!nested) {
        crimsonfs_unmount();
        return;
    }
    CHECK(crimsonfs_create(nested, "file") == 0);
    vfs_close(nested);

    vfs_node_t* file = vfs_open_at("../b/./file", "/a/b");
    CHECK(file != NULL && file->type == VFS_TYPE_FILE);
    if (file) vfs_close(file);
    file = vfs_open_at("../../../../a/b/file", "/a/b");
    CHECK(file != NULL);
    if (file) vfs_close(file);

    char cwd[VFS_PATH_MAX];
    CHECK(vfs_chdir("b/..", "/a", cwd, sizeof(cwd)) == 0);
    CHECK(strcmp(cwd, "/a") == 0);
    CHECK(vfs_chdir("../a/b", "/a", cwd, sizeof(cwd)) == 0);
    CHECK(strcmp(cwd, "/a/b") == 0);
    CHECK(vfs_chdir("file", "/a/b", cwd, sizeof(cwd)) != 0);

    CHECK(vfs_unlink_at("b", "/a") != 0);
    CHECK(vfs_unlink_at("file", "/a/b") == 0);
    CHECK(vfs_unlink_at("b", "/a") == 0);
    CHECK(vfs_unlink_at("a", "/") == 0);
    CHECK(vfs_open("/a") == NULL);
    CHECK(crimsonfs_unmount() == 0);
}

int main(void) {
    for (size_t i = 0; i < FAKE_BLOCKS; i++) {
        for (size_t j = 0; j < BLKDEV_BLOCK_SIZE; j++) backing[i][j] = 0x55;
    }

    test_format_and_empty_mount();
    test_create_write_read();
    test_persistence_across_mounts();
    test_capacity_clamps();
    test_vfs_file_seek_and_access_modes();
    test_unlink_removes_file_and_frees_blocks();
    test_creation_and_modification_timestamps();
    test_nested_directories_and_paths();

    printf("%s: %d checks, %d failures\n", "crimsonfs_test", checks, failures);
    return failures ? 1 : 0;
}
