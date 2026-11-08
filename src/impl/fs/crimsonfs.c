/*
 * CrimsonFS driver: format, mount and node operations over the generic
 * block device layer.
 *
 * Invariants that keep the logic small:
 *   - Directory sizes are always whole blocks, so `size / 512` blocks hold
 *     `size / 64` slots.
 *   - The allocation bitmap and inode table are cached in RAM at mount time
 *     and written back synchronously inside every mutating operation.
 *   - File content blocks are fetched with read-modify-write splicing.
 *
 * One volume is supported at a time; crimsonfs_unmount() frees the caches.
 */
#include "fs/crimsonfs.h"
#include "fs/vfs.h"
#include "print.h"
#include "mm/kmalloc.h"
#include "string.h"

typedef struct {
    uint32_t ino;      /* inode table index */
    uint64_t pos;      /* file cursor for reads/writes through vfs */
} cs_file_t;

static crimsonfs_mount_t* fs_mount = NULL;   /* NULL until mount succeeds */
static vfs_node_t* fs_root_node = NULL;

#ifndef MIN_SIZE
#define MIN_SIZE(a, b) ((a) < (b) ? (a) : (b))
#endif

/* ---------------- small blocks ---------------- */

static int dev_read(uint32_t lba, uint32_t count, void* buf) {
    if (!fs_mount || !buf || !fs_mount->dev->read_blocks ||
        lba > fs_mount->super.total_blocks ||
        count > fs_mount->super.total_blocks - lba) return -1;
    return fs_mount->dev->read_blocks(fs_mount->dev, lba, count, buf);
}

static int dev_write(uint32_t lba, uint32_t count, const void* buf) {
    if (!fs_mount || !buf || !fs_mount->dev->write_blocks ||
        lba > fs_mount->super.total_blocks ||
        count > fs_mount->super.total_blocks - lba) return -1;
    return fs_mount->dev->write_blocks(fs_mount->dev, lba, count, buf);
}

static int data_lba_valid(uint32_t lba) {
    return fs_mount && lba >= CRIMSONFS_DATA_LBA &&
           lba < fs_mount->super.total_blocks &&
           lba < CRIMSONFS_BITMAP_MAX_BLOCKS;
}

static void zero8(void* p, size_t n) {
    uint8_t* q = (uint8_t*)p;
    for (size_t i = 0; i < n; i++) q[i] = 0;
}

/* ---------------- allocation bitmap ---------------- */

static int bitmap_test(uint32_t index) {
    if (index >= CRIMSONFS_BITMAP_MAX_BLOCKS) return 1;
    return (fs_mount->bitmap[index >> 3] >> (index & 7)) & 1;
}

static void bitmap_set(uint32_t index) {
    if (index >= CRIMSONFS_BITMAP_MAX_BLOCKS) return;
    fs_mount->bitmap[index >> 3] |= (uint8_t)(1u << (index & 7));
}

static void bitmap_clear(uint32_t index) {
    if (index >= CRIMSONFS_BITMAP_MAX_BLOCKS) return;
    fs_mount->bitmap[index >> 3] &= (uint8_t)~(1u << (index & 7));
}

static int bitmap_flush(void) {
    return dev_write(CRIMSONFS_BITMAP_LBA, CRIMSONFS_BITMAP_BLOCKS, fs_mount->bitmap);
}

static int inodes_flush(void) {
    return dev_write(CRIMSONFS_INODE_LBA, CRIMSONFS_INODE_BLOCKS, fs_mount->inodes);
}

static int super_flush(void) {
    uint8_t scratch[CRIMSONFS_BLOCK_SIZE];
    zero8(scratch, sizeof(scratch));
    for (size_t i = 0; i < sizeof(crimsonfs_super_t); i++) {
        scratch[i] = ((uint8_t*)&fs_mount->super)[i];
    }
    return dev_write(0, 1, scratch);
}

/* Returns a free data-block LBA and marks it used in the RAM bitmap. */
static int32_t block_alloc(void) {
    if (fs_mount->super.free_blocks == 0) return -1;
    for (uint32_t b = CRIMSONFS_DATA_LBA; b < fs_mount->super.total_blocks; b++) {
        if (!bitmap_test(b)) {
            bitmap_set(b);
            fs_mount->super.free_blocks--;
            return (int32_t)b;
        }
    }
    return -1;
}

static void block_free(uint32_t lba_index) {
    if (!fs_mount || lba_index < CRIMSONFS_DATA_LBA ||
        lba_index >= fs_mount->super.total_blocks ||
        lba_index >= CRIMSONFS_BITMAP_MAX_BLOCKS) return;
    if (bitmap_test(lba_index)) {
        bitmap_clear(lba_index);
        if (fs_mount->super.free_blocks <
            fs_mount->super.total_blocks - CRIMSONFS_DATA_LBA) {
            fs_mount->super.free_blocks++;
        }
    }
}

/* ---------------- inode content I/O ---------------- */

static crimsonfs_inode_t* inode_get(uint32_t ino) {
    if (!fs_mount || ino == 0 || ino >= CRIMSONFS_INODE_COUNT) return NULL;
    return &fs_mount->inodes[ino];
}

/* Read up to `count` bytes of inode content at `offset`, clamping to EOF.
 * Holes (missing blocks) read as zeros. Returns bytes moved or -1 on I/O
 * failure before any byte was produced. */
static int inode_pread(crimsonfs_inode_t* in, uint64_t offset, void* buf, size_t count) {
    if (!in || (count && !buf) || in->size > CRIMSONFS_MAX_FILE) return -1;
    if (offset >= in->size) return 0;
    count = MIN_SIZE(count, (size_t)(in->size - offset));

    uint8_t* out = (uint8_t*)buf;
    uint8_t block[CRIMSONFS_BLOCK_SIZE];
    size_t done = 0;
    while (done < count) {
        uint64_t pos = offset + done;
        size_t bi = (size_t)(pos / CRIMSONFS_BLOCK_SIZE);
        size_t in_blk = (size_t)(pos & (CRIMSONFS_BLOCK_SIZE - 1));
        size_t take = CRIMSONFS_BLOCK_SIZE - in_blk;
        if (take > count - done) take = count - done;
        if (bi >= CRIMSONFS_DIRECT_BLOCKS + CRIMSONFS_INDIRECT_BLOCKS) break;

        uint32_t bno = 0;
        if (bi < CRIMSONFS_DIRECT_BLOCKS) {
            bno = in->direct[bi];
        } else if (in->indirect != 0) {
            uint32_t table[CRIMSONFS_INDIRECT_BLOCKS];
            size_t idx = bi - CRIMSONFS_DIRECT_BLOCKS;
            if (!data_lba_valid(in->indirect)) return done ? (int)done : -1;
            if (dev_read(in->indirect, 1, table) != 0) {
                return done ? (int)done : -1;
            }
            bno = table[idx];
        }

        if (bno == 0) {
            zero8(out + done, take);
        } else {
            if (!data_lba_valid(bno)) return done ? (int)done : -1;
            if (dev_read(bno, 1, block) != 0) {
                return done ? (int)done : -1;
            }
            for (size_t i = 0; i < take; i++) out[done + i] = block[in_blk + i];
        }
        done += take;
    }
    return (int)done;
}

/* Write `count` bytes at `offset`, extending the file and allocating blocks
 * as needed. RAM inode is updated; caller flushes metadata. */
static int inode_pwrite(crimsonfs_inode_t* in, uint64_t offset, const void* buf, size_t count) {
    if (!in || (count && !buf) || in->size > CRIMSONFS_MAX_FILE ||
        offset > CRIMSONFS_MAX_FILE) return -1;
    if (offset == CRIMSONFS_MAX_FILE) return 0;
    if (count > CRIMSONFS_MAX_FILE - offset) count = CRIMSONFS_MAX_FILE - (size_t)offset;
    uint8_t block[CRIMSONFS_BLOCK_SIZE];
    size_t done = 0;

    while (done < count) {
        uint64_t pos = offset + done;
        size_t bi = (size_t)(pos / CRIMSONFS_BLOCK_SIZE);
        size_t in_blk = (size_t)(pos & (CRIMSONFS_BLOCK_SIZE - 1));
        size_t take = CRIMSONFS_BLOCK_SIZE - in_blk;
        if (take > count - done) take = count - done;
        if (bi >= CRIMSONFS_DIRECT_BLOCKS + CRIMSONFS_INDIRECT_BLOCKS) break;

        uint32_t bno = 0;
        if (bi < CRIMSONFS_DIRECT_BLOCKS) {
            bno = in->direct[bi];
        } else {
            size_t idx = bi - CRIMSONFS_DIRECT_BLOCKS;
            if (in->indirect == 0) {
                int32_t nb = block_alloc();
                if (nb < 0) break;
                in->indirect = (uint32_t)nb;
                uint32_t indirect_table[CRIMSONFS_INDIRECT_BLOCKS];
                zero8(indirect_table, sizeof(indirect_table));
                if (dev_write(in->indirect, 1, indirect_table) != 0) {
                    block_free(in->indirect);
                    in->indirect = 0;
                    break;
                }
            }

            uint32_t indirect_table[CRIMSONFS_INDIRECT_BLOCKS];
            if (!data_lba_valid(in->indirect)) break;
            if (dev_read(in->indirect, 1, indirect_table) != 0) break;
            bno = indirect_table[idx];
            if (bno == 0) {
                int32_t nb = block_alloc();
                if (nb < 0) break;
                bno = (uint32_t)nb;
                indirect_table[idx] = bno;
                if (dev_write(in->indirect, 1, indirect_table) != 0) {
                    block_free(bno);
                    break;
                }
            }
        }

        if (bno == 0) {
            int32_t nb = block_alloc();
            if (nb < 0) break;
            bno = (uint32_t)nb;
            if (bi < CRIMSONFS_DIRECT_BLOCKS) {
                in->direct[bi] = bno;
            } else {
                uint32_t indirect_table[CRIMSONFS_INDIRECT_BLOCKS];
                size_t idx = bi - CRIMSONFS_DIRECT_BLOCKS;
                if (dev_read(in->indirect, 1, indirect_table) != 0) {
                    block_free(bno);
                    break;
                }
                indirect_table[idx] = bno;
                if (dev_write(in->indirect, 1, indirect_table) != 0) {
                    block_free(bno);
                    break;
                }
            }
            zero8(block, sizeof(block));
        } else if (!data_lba_valid(bno) || dev_read(bno, 1, block) != 0) {
            break;
        }

        const uint8_t* src = (const uint8_t*)buf + done;
        for (size_t i = 0; i < take; i++) block[in_blk + i] = src[i];
        if (dev_write(bno, 1, block) != 0) break;

        done += take;
        uint64_t end = offset + done;
        if (end > in->size) in->size = (uint32_t)end;
    }
    return (int)done;
}

/* ---------------- directory traversal ---------------- */

typedef int (*dirent_cb_t)(crimsonfs_dirent_t* ent, void* ctx);
#define CS_SCAN_CONTINUE 1
#define CS_SCAN_STOP     0
#define CS_SCAN_ERROR   -1

static int dir_scan(uint32_t dir_ino, dirent_cb_t cb, void* ctx) {
    crimsonfs_inode_t* in = inode_get(dir_ino);
    if (!in || in->type != CRIMFS_TYPE_DIR) return CS_SCAN_ERROR;

    size_t per_block = CRIMSONFS_BLOCK_SIZE / sizeof(crimsonfs_dirent_t);
    uint64_t blocks = (in->size + CRIMSONFS_BLOCK_SIZE - 1) / CRIMSONFS_BLOCK_SIZE;
    uint8_t block[CRIMSONFS_BLOCK_SIZE];

    for (size_t bi = 0; bi < blocks && bi < CRIMSONFS_DIRECT_BLOCKS; bi++) {
        if (in->direct[bi] == 0) continue;
        if (!data_lba_valid(in->direct[bi])) return CS_SCAN_ERROR;
        if (dev_read(in->direct[bi], 1, block) != 0) return CS_SCAN_ERROR;
        for (size_t s = 0; s < per_block; s++) {
            crimsonfs_dirent_t* ent =
                (crimsonfs_dirent_t*)(block + s * sizeof(crimsonfs_dirent_t));
            if (ent->inode == 0) continue;
            int r = cb(ent, ctx);
            if (r == CS_SCAN_STOP) return CS_SCAN_STOP;
            if (r == CS_SCAN_ERROR) return CS_SCAN_ERROR;
        }
    }
    return CS_SCAN_CONTINUE;
}

static int find_cb(crimsonfs_dirent_t* ent, void* raw) {
    struct { const char* name; size_t len; uint32_t found; }* ctx = raw;
    if (ent->name_len == ctx->len && ent->name_len <= sizeof(ent->name)) {
        size_t i = 0;
        while (i < ctx->len && ent->name[i] == ctx->name[i]) i++;
        if (i != ctx->len) return CS_SCAN_CONTINUE;
        ctx->found = ent->inode;
        return CS_SCAN_STOP;
    }
    return CS_SCAN_CONTINUE;
}

static int dir_find(uint32_t dir_ino, const char* name, uint32_t* out_ino) {
    if (!name) return -1;
    size_t len = strlen(name);
    struct { const char* name; size_t len; uint32_t found; } ctx = { name, len, 0 };
    int r = dir_scan(dir_ino, find_cb, &ctx);
    if (r == CS_SCAN_ERROR) return -1;
    if (out_ino) *out_ino = ctx.found;
    return ctx.found ? (int)ctx.found : 0;
}

/* ---------------- vfs node operations ---------------- */

static int cs_node_close(vfs_node_t* node);
static int cs_node_read(vfs_node_t* node, void* buf, size_t count);
static int cs_node_write(vfs_node_t* node, const void* buf, size_t count);
static int cs_node_seek(vfs_node_t* node, int64_t offset, int whence, uint64_t* new_offset);
static vfs_node_t* cs_lookup(vfs_node_t* dir, const char* name);

/* Build a fresh node for `ino`; `name` is heap-copied (freed on close). */
static vfs_node_t* cs_node_for(uint32_t ino, const char* name) {
    crimsonfs_inode_t* in = inode_get(ino);
    if (!in || !name) return NULL;

    size_t len = strlen(name);
    if (len > 60) return NULL;

    vfs_node_t* node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    cs_file_t* f = (cs_file_t*)kmalloc(sizeof(cs_file_t));
    uint8_t* nm = (uint8_t*)kmalloc(len + 1);
    if (!node || !f || !nm) {
        kfree(node);
        kfree(f);
        kfree(nm);
        return NULL;
    }

    f->ino = ino;
    f->pos = 0;

    for (size_t i = 0; i < len; i++) nm[i] = (uint8_t)name[i];
    nm[len] = 0;

    node->name = (const char*)nm;
    node->type = (in->type == CRIMFS_TYPE_DIR) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    node->size = in->size;
    node->data = f;
    node->lookup = (in->type == CRIMFS_TYPE_DIR) ? cs_lookup : NULL;
    node->unlink = (in->type == CRIMFS_TYPE_DIR) ? crimsonfs_unlink : NULL;
    node->open = NULL;
    node->close = cs_node_close;
    node->read = cs_node_read;
    node->write = cs_node_write;
    node->seek = cs_node_seek;
    return node;
}

static int cs_node_close(vfs_node_t* node) {
    if (!node) return -1;
    if (node->data) {
        kfree(node->data);
        node->data = 0;
    }
    if (node->name) {
        kfree((void*)node->name);
        node->name = 0;
    }
    return 0;
}

static int cs_node_read(vfs_node_t* node, void* buf, size_t count) {
    if (!node || !node->data || (count && !buf)) return -1;
    cs_file_t* f = (cs_file_t*)node->data;
    crimsonfs_inode_t* in = inode_get(f->ino);
    if (!in) return -1;
    int n = inode_pread(in, f->pos, buf, count);
    if (n > 0) {
        f->pos += (uint64_t)n;
        node->size = in->size;
    }
    return n;
}

static int cs_node_write(vfs_node_t* node, const void* buf, size_t count) {
    if (!node || !node->data || (count && !buf)) return -1;
    cs_file_t* f = (cs_file_t*)node->data;
    crimsonfs_inode_t* in = inode_get(f->ino);
    if (!in || in->type == CRIMFS_TYPE_DIR) return -1;
    int n = inode_pwrite(in, f->pos, buf, count);
    if (n > 0) {
        if (bitmap_flush() != 0 || inodes_flush() != 0 || super_flush() != 0) return -1;
        f->pos += (uint64_t)n;
        node->size = in->size;
    }
    return n;
}

static int cs_node_seek(vfs_node_t* node, int64_t offset, int whence, uint64_t* new_offset) {
    if (!node || !node->data || !new_offset) return -1;
    cs_file_t* file = (cs_file_t*)node->data;
    crimsonfs_inode_t* inode = inode_get(file->ino);
    if (!inode || inode->type == CRIMFS_TYPE_DIR ||
        inode->size > CRIMSONFS_MAX_FILE) return -1;

    uint64_t base;
    if (whence == VFS_SEEK_SET) base = 0;
    else if (whence == VFS_SEEK_CUR) base = file->pos;
    else if (whence == VFS_SEEK_END) base = inode->size;
    else return -1;

    uint64_t result;
    if (offset < 0) {
        uint64_t magnitude = (uint64_t)(-(offset + 1)) + 1;
        if (magnitude > base) return -1;
        result = base - magnitude;
    } else {
        if ((uint64_t)offset > CRIMSONFS_MAX_FILE - base) return -1;
        result = base + (uint64_t)offset;
    }

    if (result > CRIMSONFS_MAX_FILE) return -1;
    file->pos = result;
    *new_offset = result;
    return 0;
}

static vfs_node_t* cs_lookup(vfs_node_t* dir, const char* name) {
    if (!fs_mount || !dir || !dir->data || !name || dir->type != VFS_TYPE_DIR) return NULL;
    size_t len = strlen(name);
    if (len > 55) return NULL;

    cs_file_t* d = (cs_file_t*)dir->data;

    /* "" resolves the directory itself (used by vfs_open for "/"). */
    if (len == 0) return cs_node_for(d->ino, "/");

    int r = dir_find(d->ino, name, 0);
    if (r <= 0) return NULL;
    return cs_node_for((uint32_t)r, name);
}

/* ---------------- public API ---------------- */

void crimsonfs_init(void) {
    fs_mount = NULL;
    fs_root_node = NULL;
}

int crimsonfs_unmount(void) {
    if (!fs_mount) return -1;
    if (fs_root_node) {
        vfs_close(fs_root_node);
        fs_root_node = NULL;
    }
    kfree(fs_mount->bitmap);
    kfree(fs_mount->inodes);
    kfree(fs_mount);
    fs_mount = NULL;
    vfs_set_root(NULL);
    return 0;
}

int crimsonfs_format(blkdev_t* dev) {
    if (!dev || !dev->write_blocks) return -1;

    uint8_t block[CRIMSONFS_BLOCK_SIZE];
    uint32_t total = (dev->block_count < CRIMSONFS_BITMAP_MAX_BLOCKS)
                     ? (uint32_t)dev->block_count : CRIMSONFS_BITMAP_MAX_BLOCKS;
    if (total <= CRIMSONFS_DATA_LBA) return -1;

    /* 1. allocation bitmap: metadata LBAs [0, DATA_LBA) reserved */
    uint8_t* bmp = (uint8_t*)kmalloc(CRIMSONFS_BITMAP_BLOCKS * CRIMSONFS_BLOCK_SIZE);
    if (!bmp) return -1;
    zero8(bmp, CRIMSONFS_BITMAP_BLOCKS * CRIMSONFS_BLOCK_SIZE);
    for (uint32_t b = 0; b < CRIMSONFS_DATA_LBA; b++) {
        bmp[b >> 3] |= (uint8_t)(1u << (b & 7));
    }
    int rc = dev->write_blocks(dev, CRIMSONFS_BITMAP_LBA, CRIMSONFS_BITMAP_BLOCKS, bmp);
    kfree(bmp);
    if (rc != 0) return -1;

    /* 2. empty inode table + root directory inode (inode 1) */
    crimsonfs_inode_t* tab =
        (crimsonfs_inode_t*)kmalloc(CRIMSONFS_INODE_BLOCKS * CRIMSONFS_BLOCK_SIZE);
    if (!tab) return -1;
    zero8(tab, CRIMSONFS_INODE_BLOCKS * CRIMSONFS_BLOCK_SIZE);
    tab[1].type = CRIMFS_TYPE_DIR;
    tab[1].nlink = 1;
    rc = dev->write_blocks(dev, CRIMSONFS_INODE_LBA, CRIMSONFS_INODE_BLOCKS, tab);
    kfree(tab);
    if (rc != 0) return -1;

    /* 3. superblock */
    crimsonfs_super_t sb;
    zero8(&sb, sizeof(sb));
    for (size_t i = 0; i < 7; i++) sb.magic[i] = "CRIMFS0"[i];
    sb.magic[7] = '\0';
    sb.version = CRIMSONFS_VERSION;
    sb.block_size = CRIMSONFS_BLOCK_SIZE;
    sb.total_blocks = total;
    sb.bitmap_lba = CRIMSONFS_BITMAP_LBA;
    sb.bitmap_blocks = CRIMSONFS_BITMAP_BLOCKS;
    sb.inode_count = CRIMSONFS_INODE_COUNT;
    sb.inode_lba = CRIMSONFS_INODE_LBA;
    sb.inode_blocks = CRIMSONFS_INODE_BLOCKS;
    sb.data_lba = CRIMSONFS_DATA_LBA;
    sb.free_inodes = CRIMSONFS_INODE_COUNT - 1;   /* root has inode 1 */
    sb.free_blocks = total - CRIMSONFS_DATA_LBA;
    sb.root_inode = 1;
    for (size_t i = 0; i < sizeof("crimsonx") - 1 && i < sizeof(sb.label); i++) {
        sb.label[i] = "crimsonx"[i];
    }

    zero8(block, sizeof(block));
    for (size_t i = 0; i < sizeof(sb); i++) block[i] = ((uint8_t*)&sb)[i];
    if (dev->write_blocks(dev, 0, 1, block) != 0) return -1;
    return 0;
}

vfs_node_t* crimsonfs_mount(blkdev_t* dev) {
    if (!dev || !dev->read_blocks || !dev->write_blocks || fs_mount) return NULL;

    uint8_t block[CRIMSONFS_BLOCK_SIZE];
    if (dev->read_blocks(dev, 0, 1, block) != 0) return NULL;
    crimsonfs_super_t* sb = (crimsonfs_super_t*)block;
    if (sb->magic[0] != 'C' || sb->magic[1] != 'R' || sb->magic[2] != 'I' ||
        sb->magic[3] != 'M' || sb->magic[4] != 'F' || sb->magic[5] != 'S' ||
        sb->magic[6] != '0' || sb->magic[7] != '\0') return NULL;
    if (sb->version != CRIMSONFS_VERSION) return NULL;
    if (sb->block_size != CRIMSONFS_BLOCK_SIZE) return NULL;
    if (sb->total_blocks > dev->block_count || sb->total_blocks > CRIMSONFS_BITMAP_MAX_BLOCKS) return NULL;
    if (sb->bitmap_lba != CRIMSONFS_BITMAP_LBA ||
        sb->bitmap_blocks != CRIMSONFS_BITMAP_BLOCKS ||
        sb->inode_count != CRIMSONFS_INODE_COUNT ||
        sb->inode_lba != CRIMSONFS_INODE_LBA ||
        sb->inode_blocks != CRIMSONFS_INODE_BLOCKS ||
        sb->data_lba != CRIMSONFS_DATA_LBA ||
        sb->total_blocks <= sb->data_lba ||
        sb->free_blocks > sb->total_blocks - sb->data_lba ||
        sb->free_inodes >= CRIMSONFS_INODE_COUNT) return NULL;
    if (sb->root_inode == 0 || sb->root_inode >= CRIMSONFS_INODE_COUNT) return NULL;

    crimsonfs_mount_t* m = (crimsonfs_mount_t*)kmalloc(sizeof(crimsonfs_mount_t));
    if (!m) return NULL;
    m->dev = dev;
    m->bitmap = (uint8_t*)kmalloc(CRIMSONFS_BITMAP_BLOCKS * CRIMSONFS_BLOCK_SIZE);
    m->inodes = (crimsonfs_inode_t*)kmalloc(CRIMSONFS_INODE_BLOCKS * CRIMSONFS_BLOCK_SIZE);
    if (!m->bitmap || !m->inodes) {
        kfree(m->bitmap);
        kfree(m->inodes);
        kfree(m);
        return NULL;
    }

    if (dev->read_blocks(dev, sb->bitmap_lba, sb->bitmap_blocks, m->bitmap) != 0) goto fail;
    if (dev->read_blocks(dev, sb->inode_lba, sb->inode_blocks, m->inodes) != 0) goto fail;
    if (m->inodes[sb->root_inode].type != CRIMFS_TYPE_DIR) goto fail;

    /* adopt the caches only after every validation passed */
    fs_mount = m;
    for (size_t i = 0; i < sizeof(crimsonfs_super_t); i++) {
        ((uint8_t*)&fs_mount->super)[i] = block[i];
    }

    fs_root_node = cs_node_for(sb->root_inode, "/");
    if (!fs_root_node) {
        kfree(m->bitmap);
        kfree(m->inodes);
        kfree(m);
        fs_mount = NULL;
        return NULL;
    }
    fs_root_node->unlink = crimsonfs_unlink;
    vfs_set_root(fs_root_node);
    return fs_root_node;

fail:
    kfree(m->bitmap);
    kfree(m->inodes);
    kfree(m);
    return NULL;
}

int crimsonfs_create(vfs_node_t* root, const char* name) {
    if (!fs_mount || !root || !root->data || !name || root->type != VFS_TYPE_DIR) return -1;
    size_t name_len = strlen(name);
    if (name_len == 0 || name_len > 55) return -1;

    cs_file_t* d = (cs_file_t*)root->data;
    crimsonfs_inode_t* din = inode_get(d->ino);
    if (!din) return -1;

    uint32_t existing = 0;
    int lookup = dir_find(d->ino, name, &existing);
    if (lookup != 0) return -1;

    /* 1. take a free inode */
    uint32_t new_ino = 0;
    for (uint32_t i = 1; i < CRIMSONFS_INODE_COUNT; i++) {
        if (fs_mount->inodes[i].type == CRIMFS_TYPE_FREE) { new_ino = i; break; }
    }
    if (new_ino == 0) return -1;

    /* 2. destination dirent slot = end of the directory (append-only) */
    uint64_t add_pos = din->size;   /* always a multiple of 512 */
    size_t bi = (size_t)(add_pos / CRIMSONFS_BLOCK_SIZE);
    if (bi >= CRIMSONFS_DIRECT_BLOCKS) return -1;   /* directory full */

    /* build the updated dirent block first so a write failure never leaves
     * a size bump without its dirent */
    uint8_t blk[CRIMSONFS_BLOCK_SIZE];
    int allocated_dir_block = 0;
    if (din->direct[bi] == 0) {
        int32_t nb = block_alloc();
        if (nb < 0) return -1;   /* volume full */
        zero8(blk, sizeof(blk));
        din->direct[bi] = (uint32_t)nb;
        allocated_dir_block = 1;
    } else {
        if (!data_lba_valid(din->direct[bi])) return -1;
        if (dev_read(din->direct[bi], 1, blk) != 0) return -1;
    }

    crimsonfs_dirent_t* slot = (crimsonfs_dirent_t*)(blk + (add_pos & (CRIMSONFS_BLOCK_SIZE - 1)));
    zero8(slot, sizeof(*slot));
    slot->inode = new_ino;
    slot->name_len = (uint8_t)name_len;
    slot->type = CRIMFS_TYPE_FILE;
    for (size_t i = 0; i < name_len; i++) slot->name[i] = name[i];

    if (dev_write(din->direct[bi], 1, blk) != 0) {
        if (allocated_dir_block) {
            block_free(din->direct[bi]);
            din->direct[bi] = 0;
            bitmap_flush();
        }
        return -1;
    }
    din->size = (uint32_t)(add_pos + CRIMSONFS_BLOCK_SIZE);

    /* 4. initialize the file inode and commit every metadata table */
    zero8(&fs_mount->inodes[new_ino], sizeof(crimsonfs_inode_t));
    fs_mount->inodes[new_ino].type = CRIMFS_TYPE_FILE;
    fs_mount->inodes[new_ino].nlink = 1;
    fs_mount->super.free_inodes--;

    if (inodes_flush() != 0 || bitmap_flush() != 0 || super_flush() != 0) {
        return -1;
    }
    root->size = din->size;
    return 0;
}

int crimsonfs_unlink(vfs_node_t* root, const char* name) {
    if (!fs_mount || !root || !root->data || root->type != VFS_TYPE_DIR ||
        !name || name[0] == '\0') return -1;
    size_t name_len = strlen(name);
    if (name_len > 55) return -1;

    cs_file_t* d = (cs_file_t*)root->data;
    crimsonfs_inode_t* din = inode_get(d->ino);
    if (!din) return -1;

    uint32_t target_ino = 0;
    uint32_t dir_lba = 0;
    size_t slot_index = 0;
    uint8_t dir_block[CRIMSONFS_BLOCK_SIZE];
    size_t per_block = CRIMSONFS_BLOCK_SIZE / sizeof(crimsonfs_dirent_t);

    for (size_t bi = 0; bi < CRIMSONFS_DIRECT_BLOCKS; bi++) {
        if (din->direct[bi] == 0) continue;
        if (!data_lba_valid(din->direct[bi])) return -1;
        if (dev_read(din->direct[bi], 1, dir_block) != 0) return -1;

        for (size_t s = 0; s < per_block; s++) {
            crimsonfs_dirent_t* ent = (crimsonfs_dirent_t*)(dir_block + s * sizeof(crimsonfs_dirent_t));
            if (ent->inode == 0 || ent->name_len != name_len ||
                ent->name_len > sizeof(ent->name)) continue;
            size_t i = 0;
            while (i < name_len && ent->name[i] == name[i]) i++;
            if (i == name_len) {
                target_ino = ent->inode;
                dir_lba = din->direct[bi];
                slot_index = s;
                break;
            }
        }
        if (target_ino != 0) break;
    }

    if (target_ino == 0) return -1;

    crimsonfs_inode_t* target = inode_get(target_ino);
    if (!target || target->type == CRIMFS_TYPE_FREE || target->type == CRIMFS_TYPE_DIR) return -1;

    uint32_t indirect_table[CRIMSONFS_INDIRECT_BLOCKS];
    zero8(indirect_table, sizeof(indirect_table));
    if (target->indirect != 0) {
        if (target->indirect < CRIMSONFS_DATA_LBA ||
            target->indirect >= fs_mount->super.total_blocks ||
            dev_read(target->indirect, 1, indirect_table) != 0) return -1;
    }
    for (size_t i = 0; i < CRIMSONFS_DIRECT_BLOCKS; i++) {
        if (target->direct[i] != 0 &&
            (target->direct[i] < CRIMSONFS_DATA_LBA ||
             target->direct[i] >= fs_mount->super.total_blocks)) return -1;
    }
    for (size_t i = 0; i < CRIMSONFS_INDIRECT_BLOCKS; i++) {
        if (indirect_table[i] != 0 &&
            (indirect_table[i] < CRIMSONFS_DATA_LBA ||
             indirect_table[i] >= fs_mount->super.total_blocks)) return -1;
    }

    if (dev_read(dir_lba, 1, dir_block) != 0) return -1;
    crimsonfs_dirent_t* entry =
        (crimsonfs_dirent_t*)(dir_block + slot_index * sizeof(crimsonfs_dirent_t));
    if (entry->inode != target_ino || entry->name_len != name_len) return -1;
    for (size_t i = 0; i < name_len; i++) {
        if (entry->name[i] != name[i]) return -1;
    }
    crimsonfs_dirent_t saved_entry = *entry;
    zero8(entry, sizeof(*entry));
    if (dev_write(dir_lba, 1, dir_block) != 0) return -1;

    crimsonfs_inode_t saved_inode = *target;
    zero8(target, sizeof(*target));
    target->type = CRIMFS_TYPE_FREE;
    target->nlink = 0;
    target->size = 0;
    fs_mount->super.free_inodes++;
    if (inodes_flush() != 0) {
        *target = saved_inode;
        fs_mount->super.free_inodes--;
        zero8(entry, sizeof(*entry));
        *entry = saved_entry;
        dev_write(dir_lba, 1, dir_block);
        return -1;
    }

    for (size_t i = 0; i < CRIMSONFS_DIRECT_BLOCKS; i++) {
        if (saved_inode.direct[i] != 0) block_free(saved_inode.direct[i]);
    }
    if (saved_inode.indirect != 0) {
        for (size_t i = 0; i < CRIMSONFS_INDIRECT_BLOCKS; i++) {
            if (indirect_table[i] != 0) block_free(indirect_table[i]);
        }
        block_free(saved_inode.indirect);
    }

    if (bitmap_flush() != 0 || super_flush() != 0) return -1;
    root->size = din->size;
    return 0;
}
