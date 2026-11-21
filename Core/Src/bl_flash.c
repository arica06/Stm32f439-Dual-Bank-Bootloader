#include "bl_flash.h"
#include "bl_port.h"
#include "bl_layout.h"
#include "bl_config.h"
#include "stm32f4xx_hal.h"
#include <string.h>

/* All flash operations run with the bank swap cleared, so addresses and sector
 * numbers are the physical ones documented in bl_config.h. */

static void clear_flags(void)
{
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                           FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);
}

static bool erase_sectors(uint32_t first, uint32_t last)
{
    FLASH_EraseInitTypeDef e = {0};
    uint32_t err = 0;
    bool ok;

    e.TypeErase    = FLASH_TYPEERASE_SECTORS;
    e.VoltageRange = FLASH_VOLTAGE_RANGE_3;      /* 2.7-3.6 V, word programming */
    e.Sector       = first;                      /* HAL maps sectors 12-23 to bank 2 */
    e.NbSectors    = last - first + 1u;

    HAL_FLASH_Unlock();
    clear_flags();
    ok = (HAL_FLASHEx_Erase(&e, &err) == HAL_OK);
    HAL_FLASH_Lock();
    return ok;
}

bool bl_flash_erase_slot(uint32_t bank, uint32_t size)
{
    uint32_t start = BL_SLOT_ADDR(bank);
    return erase_sectors(bl_sector_for_addr(start), bl_sector_for_addr(start + size - 1u));
}

bool bl_flash_write(uint32_t addr, const void *data, uint32_t len)
{
    const uint8_t *d = (const uint8_t *)data;
    bool ok = true;

    HAL_FLASH_Unlock();
    clear_flags();
    for (uint32_t i = 0; i < len; i += 4u) {
        uint32_t w;
        memcpy(&w, d + i, 4);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + i, w) != HAL_OK) {
            ok = false;
            break;
        }
    }
    HAL_FLASH_Lock();
    return ok;
}

/* ---- metadata log port ---- */

const uint8_t *bl_port_meta_base(void)
{
    return (const uint8_t *)BL_META_ADDR;
}

int bl_port_meta_erase(void)
{
    return erase_sectors(BL_META_SECTOR, BL_META_SECTOR) ? 0 : -1;
}

int bl_port_meta_write(uint32_t offset, const void *src, uint32_t len)
{
    return bl_flash_write(BL_META_ADDR + offset, src, len) ? 0 : -1;
}
