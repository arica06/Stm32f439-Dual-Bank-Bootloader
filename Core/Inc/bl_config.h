#ifndef BL_CONFIG_H
#define BL_CONFIG_H

/*
 * STM32F439 dual-bank (A/B) layout.
 *
 * All addresses below are PHYSICAL, i.e. as seen with SYSCFG_MEMRMP.UFB_MODE = 0.
 * That is always the case while the bootloader runs (it executes from bank 1 and
 * clears the swap on every reset).
 *
 *   Bank 1  0x08000000  sectors 0-2     48 KB   bootloader (not updatable by itself)
 *           0x0800C000  sector 3        16 KB   metadata log
 *           0x08010000  sectors 4-11   960 KB   slot 0
 *   Bank 2  0x08100000  sectors 12-15   64 KB   unused
 *           0x08110000  sectors 16-23  960 KB   slot 1
 *
 * Every application is linked for BL_APP_VIRT_ADDR (0x08010000). Slot 0 runs in
 * place. To run slot 1 the bootloader sets UFB_MODE, which maps bank 2 at
 * 0x08000000, so slot 1 shows up at 0x08010000 as well: ONE binary works in both slots.
 *
 * This header must stay free of HAL includes: the application may include it too.
 */

#define BL_VERSION            0x00020000u

#define BL_BANK1_BASE         0x08000000u
#define BL_BANK2_BASE         0x08100000u
#define BL_META_ADDR          0x0800C000u
#define BL_META_SIZE          0x00004000u            /* one 16 KB sector (sector 3) */
#define BL_META_SECTOR        3u
#define BL_SLOT0_ADDR         0x08010000u
#define BL_SLOT1_ADDR         0x08110000u
#define BL_SLOT_SIZE          0x000F0000u            /* 960 KB each */
#define BL_SLOT_ADDR(bank)    ((bank) == 0u ? BL_SLOT0_ADDR : BL_SLOT1_ADDR)
#define BL_APP_VIRT_ADDR      0x08010000u

#define BL_UFB_MODE           (1u << 8)              /* SYSCFG_MEMRMP: bank 2 at 0x08000000 */

#define BL_TRIAL_TRIES        3u                     /* boot attempts for an unconfirmed image */

/* Mailbox in the last 16 bytes of SRAM: the application leaves a request there
 * and resets. Reserve this RAM in both linker scripts (RAM LENGTH = 191K). */
#define BL_MBX_ADDR           0x2002FFF0u
#define BL_MBX_MAGIC          0xB00710ADu
#define BL_MBX_CMD_STAY       1u                     /* stay in update mode */
#define BL_MBX_CMD_CONFIRM    2u                     /* running image is healthy */
#define BL_MBX_CMD_ROLLBACK   3u                     /* running image is bad: go back */

/* Nucleo-144 user button B1 (PC13, active high). Hold during reset to stay. */
#define BL_BUTTON_PORT        GPIOC
#define BL_BUTTON_PIN         GPIO_PIN_13

#define BL_LISTEN_MS          1500u                  /* window to catch the host after reset */

/* Wire protocol */
#define BL_SOF                0xA5u
#define BL_CHUNK              256u
#define BL_MAX_PAYLOAD        (BL_CHUNK + 4u)

#define BL_CMD_INFO           0x01u
#define BL_CMD_BEGIN          0x02u
#define BL_CMD_DATA           0x03u
#define BL_CMD_END            0x04u
#define BL_CMD_BOOT           0x05u
#define BL_CMD_CONFIRM        0x07u
#define BL_CMD_ROLLBACK       0x08u
#define BL_ACK                0x06u
#define BL_NACK               0x15u

#define BL_BEGIN_FLAG_NO_TRIAL 0x01u                 /* install as confirmed (no rollback safety net) */

#define BL_ERR_FRAME          1u
#define BL_ERR_STATE          2u
#define BL_ERR_PARAM          3u
#define BL_ERR_FLASH          4u
#define BL_ERR_CRC            5u
#define BL_ERR_NOAPP          6u
#define BL_ERR_IMAGE          7u
#define BL_ERR_PENDING        8u                     /* unconfirmed image: confirm or roll back first */

#endif /* BL_CONFIG_H */
