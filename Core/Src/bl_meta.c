#include "bl_meta.h"
#include "bl_crc32.h"
#include "bl_port.h"
#include <string.h>

#define REC_BYTES  ((uint32_t)sizeof(meta_rec_t))
#define REC_WORDS  (REC_BYTES / 4u)
#define REC_SLOTS  (BL_META_SIZE / REC_BYTES)

static const meta_rec_t *slot_ptr(uint32_t i)
{
    return (const meta_rec_t *)(const void *)(bl_port_meta_base() + i * REC_BYTES);
}

static uint32_t rec_crc(const meta_rec_t *r)
{
    return bl_crc32(0, r, REC_BYTES - 4u);
}

static bool rec_ok(const meta_rec_t *r)
{
    return r->magic == META_MAGIC &&
           r->rec_crc == rec_crc(r) &&
           r->active < 2u && r->fallback < 2u &&
           (r->state == META_CONFIRMED || r->state == META_TRIAL);
}

static bool slot_blank(const meta_rec_t *r)
{
    const uint32_t *w = (const uint32_t *)(const void *)r;
    for (uint32_t i = 0; i < REC_WORDS; i++) {
        if (w[i] != 0xFFFFFFFFu) return false;
    }
    return true;
}

void meta_default(meta_rec_t *rec)
{
    memset(rec, 0, sizeof *rec);
    rec->active   = 0u;
    rec->state    = META_CONFIRMED;
    rec->fallback = 0u;
}

bool meta_load(meta_rec_t *out)
{
    bool found = false;
    uint32_t best = 0;

    for (uint32_t i = 0; i < REC_SLOTS; i++) {
        const meta_rec_t *r = slot_ptr(i);
        if (rec_ok(r) && (!found || r->seq > best)) {
            best = r->seq;
            memcpy(out, r, sizeof *out);
            found = true;
        }
    }
    return found;
}

int meta_append(meta_rec_t *rec)
{
    meta_rec_t latest;
    uint32_t seq = meta_load(&latest) ? latest.seq + 1u : 1u;
    uint32_t next = 0;

    /* Write just after the last used slot; a torn slot counts as used. */
    for (uint32_t i = 0; i < REC_SLOTS; i++) {
        if (!slot_blank(slot_ptr(i))) next = i + 1u;
    }
    if (next >= REC_SLOTS) {
        /* Area full: erase and start over. The new record is written right after
         * the erase; a power cut in between leaves no state (see README). */
        if (bl_port_meta_erase() != 0) return -1;
        next = 0;
    }

    rec->magic = META_MAGIC;
    rec->seq = seq;
    rec->reserved[0] = rec->reserved[1] = rec->reserved[2] = 0xFFFFFFFFu;
    rec->rec_crc = rec_crc(rec);

    if (bl_port_meta_write(next * REC_BYTES, rec, REC_BYTES) != 0) return -2;
    return memcmp(slot_ptr(next), rec, REC_BYTES) == 0 ? 0 : -3;
}
