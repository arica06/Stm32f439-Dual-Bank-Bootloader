#ifndef BL_LAYOUT_H
#define BL_LAYOUT_H

#include <stdint.h>
#include "bl_config.h"

/* Flash sector number (0-23) containing a PHYSICAL address of an STM32F42x/F43x.
 * Bank 1: 4 x 16 KB, 1 x 64 KB, 7 x 128 KB (sectors 0-11); bank 2 repeats it (12-23). */
static inline uint32_t bl_sector_for_addr(uint32_t addr)
{
    uint32_t base = BL_BANK1_BASE;
    uint32_t first = 0u;
    uint32_t off;

    if (addr >= BL_BANK2_BASE) {
        base = BL_BANK2_BASE;
        first = 12u;
    }
    off = addr - base;

    if (off < 0x10000u) return first + off / 0x4000u;
    if (off < 0x20000u) return first + 4u;
    return first + 5u + (off - 0x20000u) / 0x20000u;
}

#endif /* BL_LAYOUT_H */
