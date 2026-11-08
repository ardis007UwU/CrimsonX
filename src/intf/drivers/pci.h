#ifndef DRIVERS_PCI_H
#define DRIVERS_PCI_H

#include <stdint.h>

typedef struct pci_device {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint8_t revision_id;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t programming_interface;
    uint8_t header_type;
    struct pci_device* next;
} pci_device_t;

/**
 * Enumerate all PCI configuration-space functions and log every present device.
 * @return Number of discovered devices, or -1 if enumeration could not finish.
 */
int pci_init(void);

/**
 * Find the first enumerated device matching both identifiers.
 * @return A stable pointer to the cached device, or NULL if it was not found.
 */
const pci_device_t* pci_get_device(uint16_t vendor_id, uint16_t device_id);

#endif
