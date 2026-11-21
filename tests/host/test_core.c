/* Host-side tests for the hardware-independent core:
 *   gcc -std=c11 -Wall -Wextra -Werror -I../../Core/Inc test_core.c \
 *       ../../Core/Src/bl_crc32.c ../../Core/Src/bl_meta.c ../../Core/Src/bl_state.c -o test_core
 */
#include <stdio.h>
#include <string.h>
#include "bl_config.h"
#include "bl_crc32.h"
#include "bl_layout.h"
#include "bl_meta.h"
#include "bl_port.h"
#include "bl_state.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail++; } } while (0)

/* ------------------------------------------------ simulated metadata flash */

static uint8_t g_mem[BL_META_SIZE] __attribute__((aligned(4)));
static int     g_erases;
static long    g_budget = -1;     /* >= 0: only this many bytes of the next write succeed */

static void sim_reset(void) { memset(g_mem, 0xFF, sizeof g_mem); g_erases = 0; g_budget = -1; }

const uint8_t *bl_port_meta_base(void) { return g_mem; }

int bl_port_meta_erase(void) { memset(g_mem, 0xFF, sizeof g_mem); g_erases++; return 0; }

int bl_port_meta_write(uint32_t off, const void *src, uint32_t len)
{
    const uint8_t *s = (const uint8_t *)src;
    uint32_t n = len;
    int rc = 0;

    if (g_budget >= 0 && (uint32_t)g_budget < len) { n = (uint32_t)g_budget; rc = -1; }
    for (uint32_t i = 0; i < n; i++) {
        if (g_mem[off + i] != 0xFF) { printf("FAIL: programming a non-erased byte\n"); g_fail++; }
        g_mem[off + i] = s[i];
    }
    g_budget = -1;
    return rc;
}

/* --------------------------------------------------------------- helpers */

static bool g_valid[2];
static bool fake_valid(uint32_t bank, const meta_rec_t *rec) { (void)rec; return g_valid[bank]; }

/* One simulated reset: decide, persist, return the decision. */
static bl_decision_t boot_once(uint32_t mbx)
{
    meta_rec_t r;
    bl_decision_t d;
    bool have = meta_load(&r);

    bl_decide(&r, have, mbx, fake_valid, &d);
    if (d.write_rec) CHECK(meta_append(&d.new_rec) == 0);
    return d;
}

static meta_rec_t current(void)
{
    meta_rec_t r;
    CHECK(meta_load(&r));
    return r;
}

/* What the bootloader's END handler writes after a verified upload. */
static void install(uint32_t bank, bool trial)
{
    meta_rec_t r;
    if (!meta_load(&r)) meta_default(&r);
    r.fallback = r.active;
    r.active = bank;
    r.size[bank] = 1000;
    r.crc[bank] = 0x1234;
    r.version[bank] = 7;
    r.state = trial ? META_TRIAL : META_CONFIRMED;
    r.tries = trial ? BL_TRIAL_TRIES : 0;
    if (!trial) r.fallback = bank;
    CHECK(meta_append(&r) == 0);
}

/* ----------------------------------------------------------------- tests */

static void test_crc(void)
{
    CHECK(bl_crc32(0, "123456789", 9) == 0xCBF43926u);        /* standard check value */
    CHECK(bl_crc32(0, "", 0) == 0u);
    uint32_t part = bl_crc32(0, "hello ", 6);
    CHECK(bl_crc32(part, "world", 5) == bl_crc32(0, "hello world", 11));
}

static void test_layout(void)
{
    CHECK(bl_sector_for_addr(0x08000000u) == 0);
    CHECK(bl_sector_for_addr(0x0800BFFFu) == 2);
    CHECK(bl_sector_for_addr(BL_META_ADDR) == BL_META_SECTOR);
    CHECK(bl_sector_for_addr(0x0800FFFFu) == 3);
    CHECK(bl_sector_for_addr(BL_SLOT0_ADDR) == 4);
    CHECK(bl_sector_for_addr(0x0801FFFFu) == 4);
    CHECK(bl_sector_for_addr(0x08020000u) == 5);
    CHECK(bl_sector_for_addr(0x080FFFFFu) == 11);
    CHECK(bl_sector_for_addr(0x08100000u) == 12);
    CHECK(bl_sector_for_addr(0x0810FFFFu) == 15);
    CHECK(bl_sector_for_addr(BL_SLOT1_ADDR) == 16);
    CHECK(bl_sector_for_addr(0x0811FFFFu) == 16);
    CHECK(bl_sector_for_addr(0x08120000u) == 17);
    CHECK(bl_sector_for_addr(0x081FFFFFu) == 23);
    CHECK(BL_SLOT0_ADDR + BL_SLOT_SIZE == BL_BANK2_BASE);
    CHECK(BL_SLOT1_ADDR + BL_SLOT_SIZE == BL_BANK2_BASE + 0x100000u);
}

static void test_meta_log(void)
{
    meta_rec_t r, out;

    sim_reset();
    CHECK(!meta_load(&out));

    meta_default(&r);
    r.active = 1; r.size[1] = 4096; r.crc[1] = 0xCAFEBABEu;
    CHECK(meta_append(&r) == 0);
    CHECK(meta_load(&out));
    CHECK(out.seq == 1 && out.active == 1 && out.size[1] == 4096 && out.crc[1] == 0xCAFEBABEu);

    /* corrupt the record: it must be ignored */
    g_mem[8] ^= 0x01;
    CHECK(!meta_load(&out));
    sim_reset();

    /* 600 appends wrap the 256-slot log twice; the newest record always wins */
    meta_default(&r);
    for (uint32_t i = 1; i <= 600; i++) {
        r.version[0] = i;
        CHECK(meta_append(&r) == 0);
        CHECK(meta_load(&out) && out.version[0] == i && out.seq == i);
    }
    CHECK(g_erases == 2);

    /* torn write: previous state survives, next append skips the torn slot */
    sim_reset();
    meta_default(&r); r.version[0] = 1;
    CHECK(meta_append(&r) == 0);
    r.version[0] = 2;
    g_budget = 20;
    CHECK(meta_append(&r) == -2);
    CHECK(meta_load(&out) && out.version[0] == 1);
    r.version[0] = 3;
    CHECK(meta_append(&r) == 0);
    CHECK(meta_load(&out) && out.version[0] == 3 && out.seq == 2);
}

static void test_decide_basic(void)
{
    meta_rec_t r;
    bl_decision_t d;

    /* blank device */
    sim_reset(); g_valid[0] = g_valid[1] = false;
    d = boot_once(0);
    CHECK(d.action == BL_UPDATE && !d.write_rec);

    /* confirmed, active valid: boot it, nothing written */
    sim_reset(); install(0, false); g_valid[0] = true; g_valid[1] = false;
    d = boot_once(0);
    CHECK(d.action == BL_BOOT && d.bank == 0 && !d.write_rec);

    /* stay requested */
    d = boot_once(BL_MBX_CMD_STAY);
    CHECK(d.action == BL_UPDATE);

    /* confirmed, active damaged, other bank valid: self-heal to the other bank */
    sim_reset(); install(0, false); g_valid[0] = false; g_valid[1] = true;
    d = boot_once(0);
    CHECK(d.action == BL_BOOT && d.bank == 1 && d.write_rec);
    r = current();
    CHECK(r.active == 1 && r.state == META_CONFIRMED);

    /* confirmed, both damaged */
    sim_reset(); install(1, false); g_valid[0] = g_valid[1] = false;
    d = boot_once(0);
    CHECK(d.action == BL_UPDATE);
}

static void test_trial_lifecycle(void)
{
    meta_rec_t r;
    bl_decision_t d;

    /* bank 0 is confirmed; a new image arrives in bank 1 as a trial */
    sim_reset(); install(0, false);
    g_valid[0] = true; g_valid[1] = true;
    install(1, true);
    r = current();
    CHECK(r.active == 1 && r.fallback == 0 && r.state == META_TRIAL && r.tries == 3);

    /* three failed attempts (the app never confirms), then rollback */
    for (uint32_t expect_tries = 2; expect_tries != (uint32_t)-1; expect_tries--) {
        d = boot_once(0);
        CHECK(d.action == BL_BOOT && d.bank == 1 && d.write_rec);
        CHECK(current().tries == expect_tries);
        if (expect_tries == 0) break;
    }
    d = boot_once(0);
    CHECK(d.action == BL_BOOT && d.bank == 0);
    r = current();
    CHECK(r.active == 0 && r.state == META_CONFIRMED && r.fallback == 0);

    /* next update goes to the failed bank again */
    CHECK(1u - r.active == 1);

    /* a trial that the app confirms */
    install(1, true);
    d = boot_once(0);
    CHECK(d.action == BL_BOOT && d.bank == 1);
    d = boot_once(BL_MBX_CMD_CONFIRM);
    CHECK(d.action == BL_BOOT && d.bank == 1);
    r = current();
    CHECK(r.state == META_CONFIRMED && r.active == 1 && r.fallback == 1);
    d = boot_once(0);
    CHECK(d.action == BL_BOOT && d.bank == 1 && !d.write_rec);

    /* the app asks for a rollback during a trial */
    install(0, true);
    d = boot_once(0);
    CHECK(d.bank == 0);
    d = boot_once(BL_MBX_CMD_ROLLBACK);
    CHECK(d.action == BL_BOOT && d.bank == 1);
    CHECK(current().active == 1 && current().state == META_CONFIRMED);

    /* a trial image that is corrupt from the start: immediate rollback */
    install(0, true);
    g_valid[0] = false;
    d = boot_once(0);
    CHECK(d.action == BL_BOOT && d.bank == 1);

    /* first installation ever: trial fails and there is nothing to fall back to */
    sim_reset(); g_valid[0] = false; g_valid[1] = true;
    install(1, true);
    CHECK(current().fallback == 0);
    for (int i = 0; i < 3; i++) CHECK(boot_once(0).action == BL_BOOT);
    d = boot_once(0);
    CHECK(d.action == BL_UPDATE && !d.write_rec);

    /* confirm request on a trial whose image is broken must not confirm it */
    sim_reset(); g_valid[0] = true; g_valid[1] = false;
    install(0, false);
    install(1, true);
    d = boot_once(BL_MBX_CMD_CONFIRM);
    CHECK(d.action == BL_BOOT && d.bank == 0);
}

int main(void)
{
    test_crc();
    test_layout();
    test_meta_log();
    test_decide_basic();
    test_trial_lifecycle();

    if (g_fail) { printf("%d check(s) FAILED\n", g_fail); return 1; }
    printf("all host tests passed\n");
    return 0;
}
