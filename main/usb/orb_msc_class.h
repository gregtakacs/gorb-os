#pragma once
/* The orb's copy of TinyUSB's MSC device class (orb_msc_class.c): its endpoint buffer
 * is set per disk window instead of being a static array every boot pays for. */
#include <stdint.h>

/** The class's endpoint buffer: internal DMA memory, word aligned, a multiple of 512
 *  and under 64 KB, set before tinyusb_driver_install and kept until it is uninstalled. */
void orb_msc_class_set_buffer(uint8_t *buf, uint32_t size);
