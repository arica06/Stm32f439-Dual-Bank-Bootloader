#ifndef BL_CRC32_H
#define BL_CRC32_H

#include <stdint.h>

/* zlib-compatible CRC32 (same as Python zlib.crc32). Start with crc = 0;
 * pass the previous result to continue over more data. */
uint32_t bl_crc32(uint32_t crc, const void *data, uint32_t len);

#endif /* BL_CRC32_H */
