#include "bl_state.h"

void bl_decide(const meta_rec_t *rec, bool have_rec, uint32_t mbx,
               bl_valid_fn valid, bl_decision_t *d)
{
    d->action    = BL_UPDATE;
    d->bank      = 0u;
    d->write_rec = false;

    if (!have_rec) {                      /* blank device */
        meta_default(&d->new_rec);
        return;
    }
    d->new_rec = *rec;

    if (mbx == BL_MBX_CMD_STAY) return;   /* explicit request for update mode */

    const uint32_t a = rec->active;
    const uint32_t f = rec->fallback;
    const bool a_ok = valid(a, rec);

    if (rec->state == META_TRIAL) {
        if (mbx == BL_MBX_CMD_CONFIRM && a_ok) {
            d->new_rec.state    = META_CONFIRMED;
            d->new_rec.tries    = 0u;
            d->new_rec.fallback = a;
            d->write_rec = true;
            d->action    = BL_BOOT;
            d->bank      = a;
            return;
        }

        const bool give_up = (mbx == BL_MBX_CMD_ROLLBACK) || rec->tries == 0u || !a_ok;
        if (!give_up) {
            d->new_rec.tries = rec->tries - 1u;   /* this boot counts as an attempt */
            d->write_rec = true;
            d->action    = BL_BOOT;
            d->bank      = a;
            return;
        }

        if (f != a && valid(f, rec)) {            /* roll back */
            d->new_rec.active   = f;
            d->new_rec.state    = META_CONFIRMED;
            d->new_rec.tries    = 0u;
            d->new_rec.fallback = f;
            d->write_rec = true;
            d->action    = BL_BOOT;
            d->bank      = f;
        }
        return;                                   /* nothing safe to run: update mode */
    }

    /* META_CONFIRMED */
    if (a_ok) {
        d->action = BL_BOOT;
        d->bank   = a;
        return;
    }

    const uint32_t o = 1u - a;                    /* active image is damaged: try the other */
    if (valid(o, rec)) {
        d->new_rec.active   = o;
        d->new_rec.fallback = o;
        d->write_rec = true;
        d->action    = BL_BOOT;
        d->bank      = o;
    }
}
