/*
 * ATA PIO Mode disk driver (LBA28 / LBA48).
 *
 * Strictly polled (busy-wait on status), no IRQ 14/15 involvement, which keeps
 * the driver independent of interrupt state. Sector-aligned transfers only:
 * callers always pass whole BLKDEV_BLOCK_SIZE (512) sectors.
 *
 * Register layout (primary channel):
 *   0x1F0 data        16-bit word port
 *   0x1F1 error / features
 *   0x1F2 sector count (8-bit for LBA28, low+high split for LBA48)
 *   0x1F3..0x1F5 LBA bits
 *   0x1F6 drive/head  (0xA0 = master, 0x40 = LBA)
 *   0x1F7 command / status
 *   0x3F6 alt status / device control
 */
#include "x86_64/ata.h"
#include "x86_64/port.h"
#include "blkdev.h"
#include "print.h"
#include "mm/kmalloc.h"
#include "kernel/irq.h"

#define ATA_TIMEOUT 2000000

#define ATA_REG_DATA             0
#define ATA_REG_ERROR            1
#define ATA_REG_SECTOR_COUNT     2
#define ATA_REG_LBA_LO           3
#define ATA_REG_LBA_MID          4
#define ATA_REG_LBA_HI           5
#define ATA_REG_DRIVE_HEAD       6
#define ATA_REG_STATUS           7

#define ATA_STATUS_BSY  0x80
#define ATA_STATUS_DRQ  0x08
#define ATA_STATUS_ERR  0x01

#define ATA_CMD_READ28      0x20
#define ATA_CMD_WRITE28     0x30
#define ATA_CMD_FLUSH28     0xE7
#define ATA_CMD_READ48      0x24
#define ATA_CMD_WRITE48     0x34
#define ATA_CMD_FLUSH48     0xEA
#define ATA_CMD_IDENTIFY    0xEC

#define ATA_DRIVE_MASTER      0xA0
#define ATA_DRIVE_SLAVE       0xB0
#define ATA_DRIVE_LBA         0x40

#define ATA_SECTOR_WORDS       (BLKDEV_BLOCK_SIZE / 2)
#define ATA_MAX_LBA28          0x0FFFFFFFULL

typedef struct {
    const char* channel_name;
    uint16_t io_base;      /* 0x1F0 or 0x170 */
    uint16_t ctrl_base;    /* 0x3F6 or 0x376 */
    int is_slave;
} ata_channel_t;

typedef struct {
    ata_channel_t bus;
    uint8_t lba48_supported;
} ata_drive_t;

static ata_drive_t ata_drives[4];   /* primary m/s, secondary m/s */
static int ata_drive_count = 0;

static inline uint8_t ata_read_status(ata_channel_t* bus) {
    return port_inb(bus->io_base + ATA_REG_STATUS);
}

/* Wait until BSY clears; returns status byte or -1-only bit pattern on timeout. */
static int ata_wait_not_busy(ata_channel_t* bus) {
    for (int32_t i = 0; i < ATA_TIMEOUT; i++) {
        uint8_t s = ata_read_status(bus);
        if (!(s & ATA_STATUS_BSY)) return s;
    }
    return -1;
}

static int ata_wait_drq(ata_channel_t* bus) {
    for (int32_t i = 0; i < ATA_TIMEOUT; i++) {
        uint8_t s = ata_read_status(bus);
        if (s & ATA_STATUS_ERR) return -1;
        if (!(s & ATA_STATUS_BSY) && (s & ATA_STATUS_DRQ)) return 0;
    }
    return -1;
}

static void ata_select_drive(ata_channel_t* bus) {
    port_outb(bus->io_base + ATA_REG_DRIVE_HEAD,
              (bus->is_slave ? ATA_DRIVE_SLAVE : ATA_DRIVE_MASTER) | ATA_DRIVE_LBA);
    for (int i = 0; i < 15; i++) port_wait();   /* 400 ns drive-select settle */
}

/* IDENTIFY DEVICE; fills `id` (256 words) or returns -1 if no drive. */
static int ata_identify(ata_channel_t* bus, uint16_t* id) {
    ata_select_drive(bus);

    uint8_t s = ata_read_status(bus);
    if (s == 0xFF) return -1;   /* floating bus */

    port_outb(bus->io_base + ATA_REG_SECTOR_COUNT, 0);
    port_outb(bus->io_base + ATA_REG_LBA_LO, 0);
    port_outb(bus->io_base + ATA_REG_LBA_MID, 0);
    port_outb(bus->io_base + ATA_REG_LBA_HI, 0);
    port_outb(bus->io_base + ATA_REG_STATUS, ATA_CMD_IDENTIFY);

    s = ata_read_status(bus);
    if (s == 0) return -1;      /* no device on this slot */

    if (ata_wait_not_busy(bus) < 0) return -1;

    /* Non-ATA (packet) devices abort with ERR and clear DRQ. */
    s = ata_read_status(bus);
    if (s & ATA_STATUS_ERR) return -1;

    if (ata_wait_drq(bus) < 0) return -1;
    for (int i = 0; i < ATA_SECTOR_WORDS; i++) {
        id[i] = port_inw(bus->io_base + ATA_REG_DATA);
    }
    return 0;
}

/*
 * Single-command transfer of `sectors` (1..256) 512-byte sectors. Data
 * direction decided by command byte; buffer must hold sectors * 512 bytes.
 */
static int ata_transfer(ata_drive_t* drive, uint64_t lba, uint16_t sectors,
                        uint8_t* buffer, int is_write) {
    ata_channel_t* bus = &drive->bus;
    ata_select_drive(bus);

    if (ata_wait_not_busy(bus) < 0) return -1;

    int ext = drive->lba48_supported;
    port_outb(bus->io_base + ATA_REG_SECTOR_COUNT, 0x00);
    if (ext) {
        /* LBA48: high bytes of LBA + high byte of sector count first */
        port_outb(bus->io_base + ATA_REG_SECTOR_COUNT, (uint8_t)(sectors >> 8));
        port_outb(bus->io_base + ATA_REG_LBA_LO,    (uint8_t)(lba >> 24));
        port_outb(bus->io_base + ATA_REG_LBA_MID,   (uint8_t)(lba >> 32));
        port_outb(bus->io_base + ATA_REG_LBA_HI,    (uint8_t)(lba >> 40));
    }
    port_outb(bus->io_base + ATA_REG_SECTOR_COUNT, (uint8_t)(sectors & 0xFF));
    port_outb(bus->io_base + ATA_REG_LBA_LO, (uint8_t)lba);
    port_outb(bus->io_base + ATA_REG_LBA_MID, (uint8_t)(lba >> 8));
    port_outb(bus->io_base + ATA_REG_LBA_HI, (uint8_t)(lba >> 16));

    if (!ext && (lba + sectors - 1) >= (1ULL << 28)) return -1;

    port_outb(bus->io_base + ATA_REG_STATUS,
              is_write ? (ext ? ATA_CMD_WRITE48 : ATA_CMD_WRITE28)
                       : (ext ? ATA_CMD_READ48  : ATA_CMD_READ28));

    for (uint16_t s = 0; s < sectors; s++) {
        uint8_t* p = buffer + (uint32_t)s * BLKDEV_BLOCK_SIZE;
        int st = ata_wait_not_busy(bus);
        if (st < 0 || (st & ATA_STATUS_ERR)) return -1;
        if (ata_wait_drq(bus) < 0) return -1;

        if (is_write) {
            for (int i = 0; i < ATA_SECTOR_WORDS; i++) {
                port_outw(bus->io_base + ATA_REG_DATA, ((uint16_t*)p)[i]);
            }
        } else {
            for (int i = 0; i < ATA_SECTOR_WORDS; i++) {
                ((uint16_t*)p)[i] = port_inw(bus->io_base + ATA_REG_DATA);
            }
        }
        if (is_write) {
            /* give the drive time to digest each sector before DRQ polling */
            if (ata_wait_not_busy(bus) < 0) return -1;
        }
    }

    if (is_write) {
        port_outb(bus->io_base + ATA_REG_STATUS, ext ? ATA_CMD_FLUSH48 : ATA_CMD_FLUSH28);
        uint8_t fl = 0;
        for (int32_t i = 0; i < ATA_TIMEOUT; i++) {
            fl = ata_read_status(bus);
            if (!(fl & (ATA_STATUS_BSY | ATA_STATUS_DRQ))) break;
        }
        if (fl & ATA_STATUS_ERR) return -1;
    }
    /* Check the final status for read errors */
    {
        int st;
        for (int32_t i = 0; i < 64; i++) port_wait();
        st = ata_read_status(bus);
        if (st & ATA_STATUS_ERR) return -1;
    }
    return 0;
}

static int ata_read_blocks(blkdev_t* dev, uint64_t lba, uint32_t count, void* buf) {
    ata_drive_t* drive = (ata_drive_t*)dev->priv;
    if (count == 0) return 0;
    if (!drive->lba48_supported && (uint64_t)lba + count > (uint64_t)ATA_MAX_LBA28 + 1) return -1;

    irq_flags_t f = irq_save();
    int rc = 0;
    uint8_t* p = (uint8_t*)buf;
    while (rc == 0 && count > 0) {
        /* ATA28 caps a transfer at 256 sectors (count 0 == 256 in hardware);
         * chunk to 128 to stay far from the edge on both protocols. */
        uint32_t now = count > 128 ? 128 : count;
        if (!drive->lba48_supported && (uint64_t)lba + now > (uint64_t)ATA_MAX_LBA28 + 1) {
            now = (uint32_t)(ATA_MAX_LBA28 + 1 - lba);
        }
        rc = ata_transfer(drive, lba, (uint16_t)now, p, 0);
        lba += now;
        p += (size_t)now * BLKDEV_BLOCK_SIZE;
        count -= now;
    }
    irq_restore(f);
    return rc;
}

static int ata_write_blocks(blkdev_t* dev, uint64_t lba, uint32_t count, const void* buf) {
    ata_drive_t* drive = (ata_drive_t*)dev->priv;
    if (count == 0) return 0;
    if (!drive->lba48_supported && (uint64_t)lba + count > (uint64_t)ATA_MAX_LBA28 + 1) return -1;

    irq_flags_t f = irq_save();
    int rc = 0;
    uint8_t* p = (uint8_t*)buf;
    while (rc == 0 && count > 0) {
        uint32_t now = count > 128 ? 128 : count;
        if (!drive->lba48_supported && (uint64_t)lba + now > (uint64_t)ATA_MAX_LBA28 + 1) {
            now = (uint32_t)(ATA_MAX_LBA28 + 1 - lba);
        }
        rc = ata_transfer(drive, lba, (uint16_t)now, p, 1);
        lba += now;
        p += (size_t)now * BLKDEV_BLOCK_SIZE;
        count -= now;
    }
    irq_restore(f);
    return rc;
}

static void ata_register(ata_channel_t* bus, const char* name) {
    uint16_t id[ATA_SECTOR_WORDS];
    if (ata_identify(bus, id) != 0) return;

    /* Word 83 bit 10 = LBA48 supported */
    uint8_t lba48 = (id[83] & (1 << 10)) != 0;
    uint64_t blocks = ((uint64_t)id[61] << 16) | id[60];      /* LBA28 capacity */
    if (lba48) {
        blocks = ((uint64_t)id[103] << 48) | ((uint64_t)id[102] << 32)
               | ((uint64_t)id[101] << 16) | id[100];
    }
    if (blocks == 0) return;

    ata_drive_t* drive = &ata_drives[ata_drive_count++];
    drive->bus = *bus;
    drive->lba48_supported = lba48;

    blkdev_t* dev = (blkdev_t*)kmalloc(sizeof(blkdev_t));
    if (!dev) return;
    dev->name = name;
    dev->block_count = blocks;
    dev->read_blocks = ata_read_blocks;
    dev->write_blocks = ata_write_blocks;
    dev->priv = drive;
    dev->next = NULL;
    blkdev_register(dev);

    print_set_color(PRINT_COLOR_CYAN, PRINT_COLOR_BLACK);
    print_str("[ata] ");
    print_str(name);
    print_str(": ");
    print_uint64_dec(blocks * 512 / (1024 * 1024));
    print_str(" MB");
    if (lba48) print_str(" (LBA48)");
    print_str("\n");
}

void ata_init(void) {
    /* probe order: primary master, primary slave, secondary master, secondary slave */
    ata_channel_t b;
    const char* names[4] = { "ata-p0m0", "ata-p0s0", "ata-p1m0", "ata-p1s0" };

    b.channel_name = "primary"; b.io_base = 0x1F0; b.ctrl_base = 0x3F6; b.is_slave = 0;
    ata_register(&b, names[0]);
    b.is_slave = 1;
    ata_register(&b, names[1]);
    b.io_base = 0x170; b.ctrl_base = 0x376; b.is_slave = 0;
    ata_register(&b, names[2]);
    b.is_slave = 1;
    ata_register(&b, names[3]);
}
