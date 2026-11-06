#ifndef PIC_H
#define PIC_H

#include <stdint.h>

void pic_init(void);
void pic_remap(void);
void pic_eoi_master(void);
void pic_eoi_slave(void);

#endif