#ifndef PIC_H
#define PIC_H

#include <stdint.h>

/** Initialize and mask the master and slave 8259 PICs. */
void pic_init(void);
/** Remap PIC interrupt vectors away from CPU exception vectors. */
void pic_remap(void);
/** Acknowledge the active master-PIC interrupt. */
void pic_eoi_master(void);
/** Acknowledge the active slave-PIC interrupt and its master cascade. */
void pic_eoi_slave(void);
/** Enable or disable one legacy PIC IRQ line (0-15). */
void pic_set_irq_enabled(uint8_t irq, int enabled);

#endif