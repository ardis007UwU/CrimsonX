#ifndef CRIMSONFS_H
#define CRIMSONFS_H

#include <stdint.h>
#include <stddef.h>
#include "blkdev.h"
#include "fs/vfs.h"

/*
 *                 C r i m s o n F S
 *       The native filesystem of CrimsonX
 *
 * A small volume format with a superblock, fixed allocation bitmap, flat inode
 * table, direct block pointers, and one single-indirect block table.
 *
 * On-disk layout (blocks are 512 bytes == one LBA):
 *
 *   LBA 0       superblock (padded to one block)
 *   LBA 1...    block allocation bitmap (fixed CrimsonFS_BITMAP_BLOCKS,
 *               supports volumes up to 65536 blocks / 32 MiB; growth:
 *               make the bitmap multiple blocks and version-bump)
 *   LBA ...     inode table (CrimsonFS_INODE_COUNT * 80 bytes)
 *   LBA ...     data blocks (allocation bitmap from here forward)
 *
 * Inodes support 16 direct and 128 indirect data blocks. Directories use
 * direct blocks containing arrays of crimsonfs_dirent_t. Inode 1 is the root;
 * paths are single-level.
 */

#define CRIMSONFS_MAGIC        "CRIMFS0"
#define CRIMSONFS_VERSION      1
#define CRIMSONFS_BLOCK_SIZE   512

#define CRIMSONFS_BITMAP_LBA     1
#define CRIMSONFS_BITMAP_BLOCKS  16
#define CRIMSONFS_BITMAP_MAX_BLOCKS  ((uint32_t)(CRIMSONFS_BITMAP_BLOCKS * \
                                CRIMSONFS_BLOCK_SIZE * 8))

#define CRIMSONFS_INODE_BLOCK_SIZE 80
#define CRIMSONFS_INODE_COUNT      64
#define CRIMSONFS_INODE_LBA \
    (CRIMSONFS_BITMAP_LBA + CRIMSONFS_BITMAP_BLOCKS)                        /* 17 */
#define CRIMSONFS_INODE_BLOCKS  10 /* 64 * 80 = 5120 = 10 * 512 */
#define CRIMSONFS_DATA_LBA \
    (CRIMSONFS_INODE_LBA + CRIMSONFS_INODE_BLOCKS)                          /* 27 */
#define CRIMSONFS_DIRECT_BLOCKS 16
#define CRIMSONFS_INDIRECT_BLOCKS 128
#define CRIMSONFS_MAX_FILE \
    ((CRIMSONFS_DIRECT_BLOCKS + CRIMSONFS_INDIRECT_BLOCKS) * CRIMSONFS_BLOCK_SIZE)

typedef struct {                 /* at LBA 0 */
    char     magic[8];           /* "CRIMFS0\0" */
    uint32_t version;
    uint32_t block_size;
    uint32_t total_blocks;      /* volume capacity in 512-byte blocks */
    uint32_t bitmap_lba;
    uint32_t bitmap_blocks;
    uint32_t inode_count;
    uint32_t inode_lba;
    uint32_t inode_blocks;
    uint32_t data_lba;
    uint32_t free_blocks;
    uint32_t free_inodes;
    uint32_t root_inode;        /* always 1 */
    char     label[16];
} crimsonfs_super_t;

typedef struct {                 /* 80 bytes */
    uint8_t  type;               /* 0 free, 1 file, 2 dir */
    uint8_t  nlink;
    uint16_t reserved;
    uint32_t size;
    uint32_t direct[CRIMSONFS_DIRECT_BLOCKS];
    uint32_t indirect;           /* LBA of an array of 128 data-block LBAs */
    uint32_t unused;
} crimsonfs_inode_t;             /* inode 0 is never used; root is 1 */

typedef struct {                 /* 64 bytes, one per slot in a dir block */
    uint32_t inode;              /* inode table index, 0 = free slot */
    uint8_t  name_len;           /* strlen(name) */
    uint8_t  type;               /* duplicated for cheap listings */
    uint8_t  reserved[2];
    char     name[56];
} crimsonfs_dirent_t;

typedef struct {                 /* mounted-volume context (RAM only) */
    blkdev_t*           dev;
    crimsonfs_super_t   super;
    uint8_t*            bitmap;      /* RAM copy, 8 KiB */
    crimsonfs_inode_t*  inodes;      /* RAM copy, 5120 bytes */
} crimsonfs_mount_t;

typedef enum {
    CRIMFS_TYPE_FREE = 0,
    CRIMFS_TYPE_FILE = 1,
    CRIMFS_TYPE_DIR  = 2,
} crimsonfs_type_t;

/* --------------------------- end of structures -------------------------- */

_Static_assert(sizeof(crimsonfs_super_t) < CRIMSONFS_BLOCK_SIZE, "superblock fits a block");
_Static_assert(sizeof(crimsonfs_inode_t) == CRIMSONFS_INODE_BLOCK_SIZE, "inode size");
_Static_assert(sizeof(crimsonfs_dirent_t) == 64, "dirent must be exactly 64 bytes");
_Static_assert(CRIMSONFS_BLOCK_SIZE % sizeof(crimsonfs_dirent_t) == 0, "dirents tile a block");
_Static_assert(sizeof(crimsonfs_mount_t) < CRIMSONFS_INODE_COUNT * CRIMSONFS_INODE_BLOCK_SIZE +
               CRIMSONFS_BITMAP_BLOCKS * CRIMSONFS_BLOCK_SIZE, "RAM caches fit volume size");

/** Initialize the single-volume mount state. */
void crimsonfs_init(void);

/** Unmount the volume and free its RAM caches; returns -1 if not mounted. */
int crimsonfs_unmount(void);

/** Format a device with an empty CrimsonFS volume and root directory. */
int crimsonfs_format(blkdev_t* dev);

/**
 * Validate and mount a volume.
 * @return Fresh root directory node, installed in the VFS, or NULL on failure.
 */
vfs_node_t* crimsonfs_mount(blkdev_t* dev);

/** Create an empty root-directory file; returns 0 on success. */
int crimsonfs_create(vfs_node_t* root, const char* name);

/** Remove a root-directory file and release its allocated data blocks. */
int crimsonfs_unlink(vfs_node_t* root, const char* name);

#endif
