#include "bootloader.h"
#include "bl_config.h"
#include "bl_crc32.h"
#include "bl_flash.h"
#include "bl_meta.h"
#include "bl_state.h"
#include <stdbool.h>
#include <string.h>

typedef struct {
    bool     active;
    uint32_t bank;      /* slot being written */
    uint32_t size;
    uint32_t crc;
    uint32_t version;
    uint32_t flags;
    uint32_t next;      /* next expected byte offset */
} xfer_t;

static UART_HandleTypeDef *g_uart;
static uint8_t    g_rx[3u + BL_MAX_PAYLOAD + 4u];
static xfer_t     g_xfer;
static meta_rec_t g_rec;        /* current metadata (default values if none) */
static bool       g_have_rec;

/* ------------------------------------------------------ image validation */

static bool sp_ok(uint32_t sp)
{
    return (sp >= 0x20000000u && sp <= 0x20030000u) ||   /* SRAM1-3 */
           (sp >= 0x10000000u && sp <= 0x10010000u);     /* CCM     */
}

/* The reset vector holds a VIRTUAL address (the images are linked for 0x08010000). */
static bool slot_vectors_ok(uint32_t bank)
{
    uint32_t sp = *(volatile uint32_t *)(BL_SLOT_ADDR(bank));
    uint32_t pc = *(volatile uint32_t *)(BL_SLOT_ADDR(bank) + 4u);
    return sp_ok(sp) &&
           pc >= BL_APP_VIRT_ADDR && pc < BL_APP_VIRT_ADDR + BL_SLOT_SIZE &&
           (pc & 1u);
}

static bool slot_size_ok(uint32_t size)
{
    return size >= 8u && size <= BL_SLOT_SIZE;
}

static bool slot_valid(uint32_t bank, const meta_rec_t *rec)
{
    uint32_t size = rec->size[bank];
    if (!slot_size_ok(size)) return false;
    if (!slot_vectors_ok(bank)) return false;
    return bl_crc32(0, (const void *)BL_SLOT_ADDR(bank), size) == rec->crc[bank];
}

static bool commit(const meta_rec_t *r)
{
    meta_rec_t t = *r;
    if (meta_append(&t) != 0) return false;
    g_rec = t;
    g_have_rec = true;
    return true;
}

/* -------------------------------------------------------------- framing  */
/* Frame: [SOF][CMD][LEN lo][LEN hi][PAYLOAD...][CRC32 LE over CMD..PAYLOAD] */

static void send_frame(uint8_t cmd, const void *p, uint16_t n)
{
    uint8_t tx[1u + 3u + 24u + 4u];
    uint32_t crc;

    if (n > 24u) return;
    tx[0] = BL_SOF;
    tx[1] = cmd;
    tx[2] = (uint8_t)(n & 0xFFu);
    tx[3] = (uint8_t)(n >> 8);
    if (n) memcpy(&tx[4], p, n);
    crc = bl_crc32(0, &tx[1], 3u + n);
    memcpy(&tx[4u + n], &crc, 4);
    HAL_UART_Transmit(g_uart, tx, 1u + 3u + n + 4u, 100);
}

static void ack(void)          { send_frame(BL_ACK, NULL, 0); }
static void nack(uint8_t code) { send_frame(BL_NACK, &code, 1); }

/* 0 = valid frame, -1 = timeout waiting for SOF, -2 = malformed/bad CRC */
static int recv_frame(uint32_t timeout_ms, uint8_t *cmd, const uint8_t **payload, uint16_t *len)
{
    uint8_t  b = 0;
    uint32_t t0 = HAL_GetTick();
    bool     got_sof = false;

    while (HAL_GetTick() - t0 < timeout_ms) {
        if (HAL_UART_Receive(g_uart, &b, 1, 10) == HAL_OK && b == BL_SOF) {
            got_sof = true;
            break;
        }
    }
    if (!got_sof) return -1;

    if (HAL_UART_Receive(g_uart, g_rx, 3, 100) != HAL_OK) return -2;

    uint16_t n = (uint16_t)(g_rx[1] | (g_rx[2] << 8));
    if (n > BL_MAX_PAYLOAD) return -2;
    if (HAL_UART_Receive(g_uart, &g_rx[3], n + 4u, 500) != HAL_OK) return -2;

    uint32_t rx_crc;
    memcpy(&rx_crc, &g_rx[3u + n], 4);
    if (bl_crc32(0, g_rx, 3u + n) != rx_crc) return -2;

    *cmd     = g_rx[0];
    *payload = &g_rx[3];
    *len     = n;
    return 0;
}

/* ------------------------------------------------- bank swap and jump   */

/*
 * Runs from RAM: remapping the banks changes what the CPU sees at the flash
 * addresses it is executing from, so this code must not live in flash.
 * Everything here is register access or forced-inline CMSIS helpers.
 */
__attribute__((section(".RamFunc"), noinline, noreturn))
static void swap_and_jump(uint32_t bank, uint32_t sp, uint32_t pc)
{
    uint32_t acr = FLASH->ACR;

    FLASH->ACR = acr & ~(FLASH_ACR_ICEN | FLASH_ACR_DCEN);          /* caches off */
    SYSCFG->MEMRMP = (SYSCFG->MEMRMP & ~BL_UFB_MODE) | (bank ? BL_UFB_MODE : 0u);
    FLASH->ACR |= FLASH_ACR_ICRST | FLASH_ACR_DCRST;                /* drop stale lines */
    FLASH->ACR &= ~(FLASH_ACR_ICRST | FLASH_ACR_DCRST);
    FLASH->ACR = acr;                                               /* caches back on */
    __DSB();
    __ISB();

    SCB->VTOR = BL_APP_VIRT_ADDR;
    __set_MSP(sp);
    ((void (*)(void))pc)();
    for (;;) { }
}

static void jump_to_bank(uint32_t bank)
{
    uint32_t sp = *(volatile uint32_t *)(BL_SLOT_ADDR(bank));
    uint32_t pc = *(volatile uint32_t *)(BL_SLOT_ADDR(bank) + 4u);

    HAL_UART_DeInit(g_uart);
    HAL_DeInit();                       /* resets peripherals, including SYSCFG */
    HAL_RCC_DeInit();
    __HAL_RCC_SYSCFG_CLK_ENABLE();      /* SYSCFG must be clocked to write MEMRMP */

    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL  = 0;
    for (int i = 0; i < 8; i++) {
        NVIC->ICER[i] = 0xFFFFFFFFu;
        NVIC->ICPR[i] = 0xFFFFFFFFu;
    }

    swap_and_jump(bank, sp, pc);
}

/* Persist the decision, then start the chosen slot. Returns only on failure. */
static void execute(const bl_decision_t *d, bool send_ack)
{
    if (d->action != BL_BOOT) return;
    if (d->write_rec && !commit(&d->new_rec)) return;   /* cannot track the trial: stay safe */
    if (send_ack) ack();
    jump_to_bank(d->bank);
}

static bool try_boot(void)
{
    bl_decision_t d;
    bl_decide(&g_rec, g_have_rec, 0u, slot_valid, &d);
    if (d.action != BL_BOOT) return false;
    execute(&d, true);
    return false;                       /* only reached if the record could not be written */
}

/* -------------------------------------------------------------- commands */

static uint8_t present_mask(void)
{
    uint8_t m = 0;
    for (uint32_t b = 0; b < 2u; b++) {
        if (g_have_rec && slot_size_ok(g_rec.size[b]) && slot_vectors_ok(b)) m |= (uint8_t)(1u << b);
    }
    if (g_have_rec) m |= 0x80u;
    return m;
}

static void dispatch(uint8_t cmd, const uint8_t *p, uint16_t n)
{
    switch (cmd) {

    case BL_CMD_INFO: {
        uint8_t  r[20];
        uint32_t v = BL_VERSION, s = BL_SLOT_SIZE;
        memcpy(&r[0], &v, 4);
        memcpy(&r[4], &s, 4);
        r[8]  = (uint8_t)g_rec.active;
        r[9]  = (uint8_t)g_rec.state;
        r[10] = (uint8_t)(g_rec.state == META_TRIAL ? g_rec.tries : 0u);
        r[11] = present_mask();
        memcpy(&r[12], &g_rec.version[0], 4);
        memcpy(&r[16], &g_rec.version[1], 4);
        send_frame(BL_ACK, r, sizeof r);
        break;
    }

    case BL_CMD_BEGIN: {
        uint32_t size, crc, ver, flags;
        if (n != 16u) { nack(BL_ERR_PARAM); break; }
        memcpy(&size,  &p[0],  4);
        memcpy(&crc,   &p[4],  4);
        memcpy(&ver,   &p[8],  4);
        memcpy(&flags, &p[12], 4);
        if (!slot_size_ok(size) || (size & 3u)) { nack(BL_ERR_PARAM); break; }
        if (g_have_rec && g_rec.state == META_TRIAL) { nack(BL_ERR_PENDING); break; }

        g_xfer.active = false;
        g_xfer.bank = 1u - g_rec.active;                 /* always the inactive slot */
        if (!bl_flash_erase_slot(g_xfer.bank, size)) { nack(BL_ERR_FLASH); break; }

        g_xfer.active  = true;
        g_xfer.size    = size;
        g_xfer.crc     = crc;
        g_xfer.version = ver;
        g_xfer.flags   = flags;
        g_xfer.next    = 0;
        ack();
        break;
    }

    case BL_CMD_DATA: {
        uint32_t off, len;
        if (!g_xfer.active)              { nack(BL_ERR_STATE); break; }
        if (n < 8u || ((n - 4u) & 3u))   { nack(BL_ERR_PARAM); break; }
        memcpy(&off, &p[0], 4);
        len = n - 4u;
        if (off + len <= g_xfer.next)    { ack(); break; }          /* duplicate (lost ACK) */
        if (off != g_xfer.next || off + len > g_xfer.size) { nack(BL_ERR_PARAM); break; }
        if (!bl_flash_write(BL_SLOT_ADDR(g_xfer.bank) + off, &p[4], len)) { nack(BL_ERR_FLASH); break; }
        g_xfer.next += len;
        ack();
        break;
    }

    case BL_CMD_END: {
        meta_rec_t r;
        uint32_t b;
        if (!g_xfer.active || g_xfer.next != g_xfer.size) { nack(BL_ERR_STATE); break; }
        b = g_xfer.bank;
        if (bl_crc32(0, (const void *)BL_SLOT_ADDR(b), g_xfer.size) != g_xfer.crc) { nack(BL_ERR_CRC); break; }
        if (!slot_vectors_ok(b)) { nack(BL_ERR_IMAGE); break; }

        r = g_rec;
        r.size[b]    = g_xfer.size;
        r.crc[b]     = g_xfer.crc;
        r.version[b] = g_xfer.version;
        r.fallback   = g_rec.active;                     /* previous image = rollback target */
        r.active     = b;
        if (g_xfer.flags & BL_BEGIN_FLAG_NO_TRIAL) {
            r.state    = META_CONFIRMED;
            r.tries    = 0u;
            r.fallback = b;
        } else {
            r.state = META_TRIAL;
            r.tries = BL_TRIAL_TRIES;
        }
        if (!commit(&r)) { nack(BL_ERR_FLASH); break; }

        g_xfer.active = false;
        ack();
        break;
    }

    case BL_CMD_CONFIRM:
    case BL_CMD_ROLLBACK: {
        bl_decision_t d;
        if (!g_have_rec || g_rec.state != META_TRIAL) { nack(BL_ERR_STATE); break; }
        bl_decide(&g_rec, true,
                  cmd == BL_CMD_CONFIRM ? BL_MBX_CMD_CONFIRM : BL_MBX_CMD_ROLLBACK,
                  slot_valid, &d);
        if (!d.write_rec || d.new_rec.state != META_CONFIRMED) { nack(BL_ERR_IMAGE); break; }
        if (!commit(&d.new_rec)) { nack(BL_ERR_FLASH); break; }
        ack();
        break;
    }

    case BL_CMD_BOOT:
        if (!try_boot()) nack(BL_ERR_NOAPP);
        break;

    default:
        nack(BL_ERR_PARAM);
        break;
    }
}

/* ------------------------------------------------------------------ entry */

static uint32_t mailbox_take(void)
{
    volatile uint32_t *m = (volatile uint32_t *)BL_MBX_ADDR;
    uint32_t cmd = (m[0] == BL_MBX_MAGIC) ? m[1] : 0u;
    m[0] = 0;
    m[1] = 0;
    return cmd;
}

static bool button_pressed(void)
{
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_GPIOC_CLK_ENABLE();
    g.Pin  = BL_BUTTON_PIN;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(BL_BUTTON_PORT, &g);
    return HAL_GPIO_ReadPin(BL_BUTTON_PORT, BL_BUTTON_PIN) == GPIO_PIN_SET;
}

void bl_run(UART_HandleTypeDef *huart)
{
    uint8_t cmd;
    const uint8_t *payload;
    uint16_t len;
    bl_decision_t d;

    g_uart = huart;

    /* Flash addresses below assume the swap is off. A system reset clears it;
     * clear it explicitly anyway. */
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    SYSCFG->MEMRMP &= ~BL_UFB_MODE;

    g_have_rec = meta_load(&g_rec);
    if (!g_have_rec) meta_default(&g_rec);

    uint32_t mbx = mailbox_take();
    if (mbx == 0u && button_pressed()) mbx = BL_MBX_CMD_STAY;

    bl_decide(&g_rec, g_have_rec, mbx, slot_valid, &d);

    if (d.action == BL_BOOT) {
        bool host_found = false;

        /* Give the host a short window to interrupt the boot (skipped when the
         * application only asked us to record a confirmation). */
        if (mbx != BL_MBX_CMD_CONFIRM) {
            uint32_t t0 = HAL_GetTick();
            while (HAL_GetTick() - t0 < BL_LISTEN_MS) {
                if (recv_frame(BL_LISTEN_MS, &cmd, &payload, &len) == 0) {
                    dispatch(cmd, payload, len);
                    host_found = true;
                    break;
                }
            }
        }
        if (!host_found) execute(&d, false);   /* returns only if the record could not be written */
    }

    for (;;) {
        int rc = recv_frame(1000, &cmd, &payload, &len);
        if (rc == 0)       dispatch(cmd, payload, len);
        else if (rc == -2) nack(BL_ERR_FRAME);
    }
}
