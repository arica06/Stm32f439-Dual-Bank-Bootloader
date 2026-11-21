#ifndef BOOTLOADER_H
#define BOOTLOADER_H

#include "stm32f4xx_hal.h"

/* Call after HAL_Init(), clock and UART init. Never returns:
 * it either starts an application slot or serves firmware updates. */
void bl_run(UART_HandleTypeDef *huart);

#endif /* BOOTLOADER_H */
