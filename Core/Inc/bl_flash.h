#ifndef BL_FLASH_H
#define BL_FLASH_H

#include <stdbool.h>
#include <stdint.h>

/* Erase the sectors a new image of `size` bytes needs in slot `bank` (0/1). */
bool bl_flash_erase_slot(uint32_t bank, uint32_t size);

/* Program `len` bytes (multiple of 4) at a PHYSICAL flash address. */
bool bl_flash_write(uint32_t addr, const void *data, uint32_t len);

#endif /* BL_FLASH_H */
