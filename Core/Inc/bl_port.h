#ifndef BL_PORT_H
#define BL_PORT_H

#include <stdint.h>

/* Hardware access needed by the metadata log. Implemented by bl_flash.c on the
 * target and by a RAM simulation in the host tests. All return 0 on success. */
const uint8_t *bl_port_meta_base(void);                                   /* BL_META_SIZE readable bytes */
int bl_port_meta_erase(void);                                             /* erase the whole area to 0xFF */
int bl_port_meta_write(uint32_t offset, const void *src, uint32_t len);   /* 4-byte aligned, area must be erased */

#endif /* BL_PORT_H */
