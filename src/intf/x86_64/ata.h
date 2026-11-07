#ifndef ATA_H
#define ATA_H

#include <stdint.h>

/* Detect IDE/ATA drives on both channels (P/S masters + slaves) and register
 * each LBA-capable drive with the block device layer. Safe to call once at
 * boot; uses PIO polling only, no interrupts. */
void ata_init(void);

#endif
