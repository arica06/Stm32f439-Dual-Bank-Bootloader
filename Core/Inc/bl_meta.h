#ifndef BL_META_H
#define BL_META_H

#include <stdbool.h>
#include <stdint.h>
#include "bl_config.h"

/*
 * Metadata log. The 16 KB metadata sector is an append-only list of 64-byte
 * records. The valid record with the highest sequence number is the current
 * state. A record is only appended, never modified, so a power cut can at worst
 * leave one torn record, which fails its CRC and is ignored. The sector is erased
 * only when all 256 slots are used.
 */

#define META_MAGIC      0x4D455441u        /* "META" */
#define META_CONFIRMED  0u
#define META_TRIAL      1u

typedef struct {
    uint32_t magic;
    uint32_t seq;          /* increases with every record */
    uint32_t active;       /* bank (0/1) to boot */
    uint32_t state;        /* META_CONFIRMED or META_TRIAL */
    uint32_t tries;        /* boot attempts left while META_TRIAL */
    uint32_t fallback;     /* bank to return to if the trial fails */
    uint32_t size[2];      /* image size per bank */
    uint32_t crc[2];       /* image CRC32 per bank */
    uint32_t version[2];   /* image version per bank (informational) */
    uint32_t reserved[3];
    uint32_t rec_crc;      /* CRC32 over all preceding words */
} meta_rec_t;

_Static_assert(sizeof(meta_rec_t) == 64u, "meta record must be 64 bytes");

/* Factory state: bank 0 active and confirmed, no images known. */
void meta_default(meta_rec_t *rec);

/* Load the newest valid record. Returns false if there is none. */
bool meta_load(meta_rec_t *out);

/* Append a record (magic, seq, reserved and CRC are filled in). 0 = ok. */
int meta_append(meta_rec_t *rec);

#endif /* BL_META_H */
