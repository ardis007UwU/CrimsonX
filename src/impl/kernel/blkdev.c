#include "blkdev.h"
#include "string.h"

static blkdev_t* dev_head = NULL;
static uint32_t dev_total = 0;

void blkdev_register(blkdev_t* dev) {
    if (!dev) return;
    dev->next = NULL;

    if (!dev_head) {
        dev_head = dev;
    } else {
        blkdev_t* tail = dev_head;
        while (tail->next) tail = tail->next;
        tail->next = dev;
    }
    dev_total++;
}

uint32_t blkdev_count(void) {
    return dev_total;
}

blkdev_t* blkdev_get(uint32_t index) {
    blkdev_t* dev = dev_head;
    uint32_t i = 0;
    while (dev) {
        if (i == index) return dev;
        dev = dev->next;
        i++;
    }
    return NULL;
}

blkdev_t* blkdev_get_by_name(const char* name) {
    blkdev_t* dev = dev_head;
    while (dev) {
        if (strcmp(dev->name, name) == 0) return dev;
        dev = dev->next;
    }
    return NULL;
}
