#ifndef BL_APP_H
#define BL_APP_H

/*
 * Helpers for the APPLICATION (header only). Include after "stm32f4xx.h".
 *
 * The application never writes flash metadata itself: it leaves a request in the
 * bootloader mailbox and resets. The bootloader (which always runs with the
 * bank swap cleared, so flash addresses are unambiguous) carries it out.
 *
 * IMPORTANT: the application must not reset SYSCFG (e.g. HAL_DeInit() or an
 * APB2 force-reset) while running, or the bank swap is undone under its feet.
 */

#include <stdint.h>
#include "bl_config.h"

/* 0 = running slot 0 (bank 1), 1 = running slot 1 (bank 2). */
static inline uint32_t bl_app_running_bank(void)
{
    return (SYSCFG->MEMRMP & BL_UFB_MODE) ? 1u : 0u;
}

static inline void bl_app_mailbox_reset(uint32_t cmd)
{
    volatile uint32_t *m = (volatile uint32_t *)BL_MBX_ADDR;
    m[0] = BL_MBX_MAGIC;
    m[1] = cmd;
    __DSB();
    NVIC_SystemReset();
}

/* Reboot into the bootloader's update mode. */
static inline void bl_app_request_update(void)   { bl_app_mailbox_reset(BL_MBX_CMD_STAY); }

/* Tell the bootloader this (trial) image works. Causes one quick reset. */
static inline void bl_app_confirm_image(void)    { bl_app_mailbox_reset(BL_MBX_CMD_CONFIRM); }

/* Tell the bootloader this (trial) image is bad: return to the previous one. */
static inline void bl_app_request_rollback(void) { bl_app_mailbox_reset(BL_MBX_CMD_ROLLBACK); }

#endif /* BL_APP_H */
