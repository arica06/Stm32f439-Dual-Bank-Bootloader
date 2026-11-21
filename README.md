# STM32F439 Dual-Bank (A/B) Bootloader

[![CI](https://github.com/arica06/Stm32f439-Dual-Bank-Bootloader/actions/workflows/ci.yml/badge.svg)](https://github.com/arica06/Stm32f439-Dual-Bank-Bootloader/actions/workflows/ci.yml)

A/B firmware updates over UART for the STM32F439 (target board: NUCLEO-F439ZI) using the
chip's two flash banks. A new image is written to the inactive bank while the old one stays
intact, boots on trial, and is rolled back automatically if the application does not confirm
it. One application binary works in both slots thanks to the hardware bank swap.

> **Status.** The hardware-independent core (CRC32, metadata log, boot decision logic) and
> the host tool are tested on a PC and in CI. The bootloader has also been tested on hardware.
> A simpler single-slot version of this bootloader is a separate repository.

## How it works

```
 Bank 1 (0x08000000)                       Bank 2 (0x08100000)
 +------------------------+                +------------------------+
 | bootloader     48 KB   |                | unused          64 KB  |
 | metadata log   16 KB   |                +------------------------+
 +------------------------+                |                        |
 | slot 0        960 KB   |                | slot 1         960 KB  |
 | (0x08010000)           |                | (0x08110000)           |
 +------------------------+                +------------------------+
```

- The bootloader always runs from bank 1 and always writes to the **inactive** slot.
- Every application is linked for `0x08010000`. Slot 0 runs in place. To run slot 1 the
  bootloader sets `SYSCFG_MEMRMP.UFB_MODE`, which maps bank 2 at `0x08000000`, so slot 1 appears
  at `0x08010000` too. The same `.bin` can be uploaded to either slot.
- The swap is a RAM-resident function (`.RamFunc`): remapping flash while executing from it
  would pull the code out from under the CPU. Instruction/data caches are flushed around it.
- A system reset clears the swap, so the bootloader at `0x08000000` always starts from bank 1.
  The bootloader clears it explicitly as well.
- All flash erase/program happens inside the bootloader with the swap cleared, so addresses and
  sector numbers are unambiguous. The application never writes flash metadata; it leaves a
  request in a RAM mailbox and resets (`bl_app.h`).

## Update, trial, confirm, rollback

1. `flash` uploads the image to the inactive slot, the bootloader verifies its CRC32 and writes a
   metadata record: **active = new slot, state = TRIAL, tries = 3, fallback = old slot**.
2. Every boot of a TRIAL image uses up one try *before* jumping, so a crash loop cannot go on forever.
3. When the application is healthy it calls `bl_app_confirm_image()`: one quick reset, the
   bootloader records **CONFIRMED** and starts the same image again (no 1.5 s wait).
4. If the tries run out, the image is corrupt, or the application calls
   `bl_app_request_rollback()`, the bootloader switches back to the fallback slot (if it is intact).
5. A new upload is refused while an image is still on trial (`pending` error), so the fallback
   cannot be overwritten before the new image has proven itself. Use `confirm` or `rollback`.

| State | Condition | Result |
|-------|-----------|--------|
| CONFIRMED | active slot intact | boot it |
| CONFIRMED | active slot damaged, other slot intact | switch to the other slot, boot it |
| TRIAL | tries left, image intact | boot, tries - 1 |
| TRIAL | confirm requested | CONFIRMED, boot |
| TRIAL | tries = 0, damaged, or rollback requested | fallback slot (if intact) as CONFIRMED, else update mode |
| any | stay requested (B1 held, `bl_app_request_update()`), no metadata, nothing bootable | update mode |

The decision function (`bl_decide`) has no hardware dependencies and is covered by the host tests.

## Metadata log

The 16 KB metadata sector (sector 3) is an append-only log of 64-byte records. The valid record
with the highest sequence number is the current state. Records are never modified, so a power
cut can leave at most one torn record, which fails its CRC and is skipped. The sector is erased
only after 256 records. Each record holds: sequence, active slot, state, tries left, fallback slot,
and size / CRC32 / version for both slots.

Known weak spot: when the log wraps, the sector is erased and the new record written right after.
A power cut in that short window loses the state; the device then comes up in update mode
(slots are untouched, but must be re-selected by uploading again). This happens once per 256 records.

## Using it in your project

| File | Purpose |
|------|---------|
| `Core/Src/main.c` | NUCLEO-F439ZI entry: 180 MHz clock, USART3 (ST-LINK VCP), LD2, calls `bl_run()` |
| `Core/Src/bootloader.c` | UART protocol, boot sequence, bank swap and jump |
| `Core/Src/bl_flash.c` | HAL flash driver, metadata port |
| `Core/Src/bl_meta.c`, `bl_state.c`, `bl_crc32.c` | Portable core (host-tested) |
| `Core/Inc/bl_config.h` | Memory map, protocol constants (no HAL includes, safe for the app) |
| `Core/Inc/bl_app.h` | Application helpers: running slot, request update/confirm/rollback |
| `startup_stm32f439xx.s` | Vector table (16 core + 91 IRQs), reset handler |
| `STM32F439ZI_FLASH_BL.ld`, `STM32F439ZI_FLASH_APP.ld` | Linker scripts: bootloader 48 KB at `0x08000000`; app 960 KB at `0x08010000` |
| `stm32f439-dual-bank-bootloader.ioc` | CubeMX file: clock tree, SWD, SysTick only; main() generation off |

**Bootloader project**

1. Open the `.ioc` in STM32CubeIDE/CubeMX and generate code, or copy `stm32f4xx_hal_conf.h`,
   `stm32f4xx_it.c` (needs `SysTick_Handler` -> `HAL_IncTick`: the bootloader's timeouts depend on
   it), `system_stm32f4xx.c` and the HAL drivers from a Cube project for the NUCLEO-F439ZI.
2. Keep this repository's `main.c`, `Core/Src/bl_*.c`, `bootloader.c`, headers, `startup_stm32f439xx.s` and
   `STM32F439ZI_FLASH_BL.ld`. Select this script in the linker settings and exclude any Cube-generated startup file from the build so only one vector table is linked.

Both linker scripts use 191 KB of SRAM at `0x20000000` and 64 KB of CCMRAM at
`0x10000000`. The last 1 KB of SRAM (`0x2002FC00`–`0x2002FFFF`) is reserved;
the mailbox is at `0x2002FFF0`. They place `.RamFunc` in `.data`, which the supplied
startup copies from flash to SRAM. The startup also calls `SystemInit`, clears `.bss`,
calls `__libc_init_array`, then enters `main`. Unimplemented interrupt handlers use
`Default_Handler`; supply a working `SysTick_Handler` for HAL timeouts.

**Application project**

- Linker script `STM32F439ZI_FLASH_APP.ld`; in `system_stm32f4xx.c` set `#define VECT_TAB_OFFSET 0x10000`.
- Build a `.bin` (`arm-none-eabi-objcopy -O binary app.elf app.bin`).
- Include `bl_config.h` / `bl_app.h` (after `stm32f4xx.h`). Once the application has proven itself
  (peripherals up, communication working), call `bl_app_confirm_image()`.
- Do **not** reset SYSCFG while running (`HAL_DeInit()`, APB2 force reset): that undoes the swap.

## Flashing

```bash
pip install pyserial
python tools/flash_tool.py COM5 info
python tools/flash_tool.py COM5 flash app.bin --version 2 --boot
python tools/flash_tool.py COM5 confirm        # or: rollback
```

Start the tool, then press RESET (or hold B1 + RESET). If your application cannot confirm yet,
use `flash --no-trial` to install the image as confirmed (no automatic rollback).

## Protocol

All values little-endian. Frame: `[0xA5][CMD][LEN lo][LEN hi][PAYLOAD][CRC32 of CMD..PAYLOAD]`.

| CMD | Name | Payload | Reply payload |
|-----|------|---------|---------------|
| `0x01` | INFO | - | bootloader version u32, slot size u32, active u8, state u8 (0 confirmed / 1 trial), tries u8, present mask u8 (bit0/1 slot present, bit7 metadata valid), version slot 0 u32, version slot 1 u32 |
| `0x02` | BEGIN | size, crc32, version, flags (u32 each; flag 1 = no trial) | - (erases the inactive slot's needed sectors) |
| `0x03` | DATA | offset u32 + up to 256 bytes | - |
| `0x04` | END | - | - (verifies CRC32, writes the metadata record) |
| `0x05` | BOOT | - | - (runs the boot decision; starts the image) |
| `0x07` | CONFIRM | - | - (trial image becomes confirmed) |
| `0x08` | ROLLBACK | - | - (return to the fallback slot) |

Replies are `0x06` (ACK) or `0x15` (NACK + error byte: 1 frame, 2 state, 3 param, 4 flash, 5 crc,
6 no bootable image, 7 bad image, 8 pending trial). DATA chunks are sequential and 4-byte
aligned; a repeated chunk (lost ACK) is acknowledged without being written twice.

## Tests and CI

- `tests/host/test_core.c`: CRC32 (including the standard check value), sector map, metadata log
  (wrap-around, corruption, torn writes), and the boot decision logic through full
  update / trial / confirm / rollback lifecycles. Runs with AddressSanitizer and UBSan.
- `tests/test_flash_tool.py`: frame handling and INFO parsing of the host tool.
- CI (GitHub Actions): the two test jobs above plus a Cortex-M4 compile check of all firmware
  sources against the ST HAL and CMSIS headers (warnings enabled).

## Limitations

- The bootloader itself cannot be updated by this mechanism.
- Integrity only (CRC32), no authenticity: no signature check yet.
- UART in polling mode; faster baud rates need DMA with idle-line detection.
- Confirmation costs one extra reset (by design: only the bootloader writes flash metadata).
- Metadata wrap-around has a small power-loss window (see above).
- Bank 2 sectors 12-15 (64 KB) are unused.

## License

This project is licensed under the [MIT License](LICENSE).
