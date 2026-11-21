#ifndef BL_STATE_H
#define BL_STATE_H

#include <stdbool.h>
#include <stdint.h>
#include "bl_meta.h"

/* Boot decision logic, free of hardware access so it can be tested on a PC. */

typedef enum { BL_UPDATE = 0, BL_BOOT = 1 } bl_action_t;

typedef struct {
    bl_action_t action;     /* BL_BOOT: run `bank`; BL_UPDATE: stay in update mode */
    uint32_t    bank;
    bool        write_rec;  /* persist new_rec before acting */
    meta_rec_t  new_rec;
} bl_decision_t;

/* Returns true if the image of `bank` described by `rec` is intact. */
typedef bool (*bl_valid_fn)(uint32_t bank, const meta_rec_t *rec);

/*
 * rec/have_rec: current metadata (have_rec = false on a blank device)
 * mbx:          request left by the application or user (BL_MBX_CMD_*, 0 = none)
 *
 *  CONFIRMED  active valid          -> boot active
 *  CONFIRMED  active broken         -> boot the other bank if valid (self-heal)
 *  TRIAL      tries left, valid     -> boot active, tries - 1
 *  TRIAL      confirm requested     -> mark CONFIRMED, boot active
 *  TRIAL      tries == 0 / broken / rollback requested
 *                                   -> back to `fallback` as CONFIRMED, if it is valid
 *  anything else                    -> update mode
 */
void bl_decide(const meta_rec_t *rec, bool have_rec, uint32_t mbx,
               bl_valid_fn valid, bl_decision_t *d);

#endif /* BL_STATE_H */
