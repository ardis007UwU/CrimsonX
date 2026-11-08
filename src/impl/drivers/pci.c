#include "drivers/pci.h"
#include "kernel/irq.h"
#include "mm/kmalloc.h"
#include "print.h"
#include "x86_64/port.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC
#define PCI_CONFIG_ENABLE  0x80000000U
#define PCI_BUS_COUNT      256U
#define PCI_DEVICE_COUNT   32U
#define PCI_FUNCTION_COUNT 8U

static pci_device_t* pci_devices;
static pci_device_t* pci_devices_tail;
static int pci_device_count;
static int pci_initialized;

static uint32_t pci_read_config32(uint8_t bus, uint8_t device,
                                  uint8_t function, uint8_t offset) {
    uint32_t address = PCI_CONFIG_ENABLE |
                       ((uint32_t)bus << 16) |
                       ((uint32_t)device << 11) |
                       ((uint32_t)function << 8) |
                       ((uint32_t)offset & 0xFCU);
    irq_flags_t flags = irq_save();
    port_outl(PCI_CONFIG_ADDRESS, address);
    uint32_t value = port_inl(PCI_CONFIG_DATA);
    irq_restore(flags);
    return value;
}

static void print_hex_byte(uint8_t value) {
    static const char digits[] = "0123456789ABCDEF";
    print_char(digits[value >> 4]);
    print_char(digits[value & 0x0F]);
}

static void print_hex_word(uint16_t value) {
    print_hex_byte((uint8_t)(value >> 8));
    print_hex_byte((uint8_t)value);
}

static void pci_log_device(const pci_device_t* device) {
    print_str("[pci] ");
    print_uint64_dec(device->bus);
    print_char(':');
    print_uint64_dec(device->device);
    print_char('.');
    print_uint64_dec(device->function);
    print_str(" vendor=");
    print_hex_word(device->vendor_id);
    print_str(" device=");
    print_hex_word(device->device_id);
    print_str(" class=");
    print_hex_byte(device->class_code);
    print_char(':');
    print_hex_byte(device->subclass);
    print_char('\n');
}

static void pci_discard_devices(void) {
    pci_device_t* device = pci_devices;
    while (device) {
        pci_device_t* next = device->next;
        kfree(device);
        device = next;
    }
    pci_devices = NULL;
    pci_devices_tail = NULL;
    pci_device_count = 0;
}

int pci_init(void) {
    if (pci_initialized) return pci_device_count;

    for (uint32_t bus = 0; bus < PCI_BUS_COUNT; bus++) {
        for (uint32_t device = 0; device < PCI_DEVICE_COUNT; device++) {
            for (uint32_t function = 0; function < PCI_FUNCTION_COUNT; function++) {
                uint32_t identifiers = pci_read_config32((uint8_t)bus,
                                                         (uint8_t)device,
                                                         (uint8_t)function,
                                                         0x00);
                uint16_t vendor_id = (uint16_t)identifiers;
                if (vendor_id == 0xFFFFU) continue;

                uint32_t class_info = pci_read_config32((uint8_t)bus,
                                                        (uint8_t)device,
                                                        (uint8_t)function,
                                                        0x08);
                uint32_t header_info = pci_read_config32((uint8_t)bus,
                                                         (uint8_t)device,
                                                         (uint8_t)function,
                                                         0x0C);
                pci_device_t* found = (pci_device_t*)kmalloc(sizeof(*found));
                if (!found) {
                    print_str("[pci] enumeration stopped: out of memory\n");
                    pci_discard_devices();
                    return -1;
                }

                found->bus = (uint8_t)bus;
                found->device = (uint8_t)device;
                found->function = (uint8_t)function;
                found->revision_id = (uint8_t)class_info;
                found->vendor_id = vendor_id;
                found->device_id = (uint16_t)(identifiers >> 16);
                found->class_code = (uint8_t)(class_info >> 24);
                found->subclass = (uint8_t)(class_info >> 16);
                found->programming_interface = (uint8_t)(class_info >> 8);
                found->header_type = (uint8_t)(header_info >> 16);
                found->next = NULL;

                if (pci_devices_tail) pci_devices_tail->next = found;
                else pci_devices = found;
                pci_devices_tail = found;
                pci_device_count++;
                pci_log_device(found);
            }
        }
    }

    print_str("[pci] discovered ");
    print_uint64_dec((uint64_t)pci_device_count);
    print_str(" functions\n");
    pci_initialized = 1;
    return pci_device_count;
}

const pci_device_t* pci_get_device(uint16_t vendor_id, uint16_t device_id) {
    for (pci_device_t* device = pci_devices; device; device = device->next) {
        if (device->vendor_id == vendor_id && device->device_id == device_id) {
            return device;
        }
    }
    return NULL;
}
