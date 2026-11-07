#ifndef BLKDEV_H
#define BLKDEV_H

#include <stdint.h>
#include <stddef.h>

/*
 * Generic block device interface.
 *
 * A block device deals in fixed-size 512-byte blocks (BLKDEV_BLOCK_SIZE),
 * aligned with ATA LBA addressing. Higher layers (CrimsonFS) never touch
 * hardware registers — they only see blkdev_t function pointers, which makes
 * the whole filesystem stack testable on the host with in-memory devices.
 */
#define BLKDEV_BLOCK_SIZE 512

typedef struct blkdev blkdev_t;
typedef int (*blkdev_op_t)(blkdev_t* dev, uint64_t lba, uint32_t count, void* buf);

struct blkdev {
    const char* name;        /* e.g. "ata-p0m0" (primary master) */
    uint64_t block_count;    /* total addressable 512-byte sectors */
    /* Both return 0 on success, -1 on error. */
    int (*read_blocks)(blkdev_t* dev, uint64_t lba, uint32_t count, void* buf);
    int (*write_blocks)(blkdev_t* dev, uint64_t lba, uint32_t count, const void* buf);
    void* priv;              /* driver-private context */
    struct blkdev* next;     /* registry linkage */
};

/* Register a device with the kernel (appends to the list). */
void blkdev_register(blkdev_t* dev);

uint32_t blkdev_count(void);
blkdev_t* blkdev_get(uint32_t index);
blkdev_t* blkdev_get_by_name(const char* name);

#endif
