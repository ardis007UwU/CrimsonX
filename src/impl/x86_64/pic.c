#include <stdint.h>
#include "x86_64/port.h"

#define PORT_PIC1_COMMAND 0x20
#define PORT_PIC1_DATA 0x21
#define PORT_PIC2_COMMAND 0xA0
#define PORT_PIC2_DATA 0xA1

#define IRQ0_TIMER 0x00
#define IRQ1_KEYBOARD 0x01
#define IRQ2_SLAVE 0x02

#define ICW1_INIT 0x10
#define ICW1_HAS_ICW4 0x01

#define ICW3_IRQ2_HAS_SLAVE (1 << IRQ2_SLAVE)

#define ICW4_MODE_8086 (1 << 0)

#define PIC_EOI 0x20
#define PIC_OFFSET_MASTER 0x20
#define PIC_OFFSET_SLAVE 0x28

void pic_eoi_master() {
    port_outb(PORT_PIC1_COMMAND, PIC_EOI);
    port_wait();
}

void pic_eoi_slave() {
    port_outb(PORT_PIC2_COMMAND, PIC_EOI);
    port_wait();
}

void pic_remap() {

    port_outb(PORT_PIC1_COMMAND, ICW1_INIT | ICW1_HAS_ICW4); // master
    port_wait();
    port_outb(PORT_PIC2_COMMAND, ICW1_INIT | ICW1_HAS_ICW4); // slave
    port_wait();
    

    port_outb(PORT_PIC1_DATA, PIC_OFFSET_MASTER);
    port_wait();
    port_outb(PORT_PIC2_DATA, PIC_OFFSET_SLAVE);
    port_wait();
    
 
    port_outb(PORT_PIC1_DATA, ICW3_IRQ2_HAS_SLAVE);
    port_wait();
    port_outb(PORT_PIC2_DATA, IRQ2_SLAVE);
    port_wait();
    
    
    port_outb(PORT_PIC1_DATA, ICW4_MODE_8086);
    port_wait();
    port_outb(PORT_PIC2_DATA, ICW4_MODE_8086);
    port_wait();
    
    
    port_outb(PORT_PIC1_DATA, 0xFF);
    port_wait();
    port_outb(PORT_PIC2_DATA, 0xFF);
    port_wait();
    
    
    uint8_t mask_master = port_inb(PORT_PIC1_DATA);
    uint8_t mask_slave = port_inb(PORT_PIC2_DATA);
    mask_master &= ~(1 << IRQ1_KEYBOARD);
    port_outb(PORT_PIC1_DATA, mask_master);
    port_wait();
    port_outb(PORT_PIC2_DATA, mask_slave);
    port_wait();
    
    
    pic_eoi_master();
    pic_eoi_slave();
}

void pic_init(void) {
    pic_remap();
}