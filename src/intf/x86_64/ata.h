#ifndef ATA_H
#define ATA_H

#include <stdint.h>

/**
 * Detect IDE/ATA drives and register supported devices with the block layer.
 * Safe to call once during boot; uses polling I/O without interrupts.
 */
void ata_init(void);

#endif
