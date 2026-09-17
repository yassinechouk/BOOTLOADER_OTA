/*
 * Test-facing API over the real firmware modules.
 *
 * Exposes just enough for a Python driver to do what the board does:
 * hand bytes to the protocol state machine, collect what it sends
 * back, and inspect flash and metadata afterwards.
 *
 * Nothing here is compiled into the firmware. The modules under test
 * -- metadata_mgr.c and protocol_mgr.c -- are built from their normal
 * sources with no test-only conditionals.
 */

#include <stdint.h>
#include <string.h>

#include "protocol_mgr.h"
#include "metadata.h"
#include "metadata_mgr.h"
#include "flash.h"
#include "crc.h"

void fake_flash_init(void);
void fake_uart_init(void);
void fake_uart_push_rx(const uint8_t *data, uint32_t len);
uint32_t fake_uart_pop_tx(uint8_t *out, uint32_t max);
extern unsigned fake_erase_count;
extern unsigned fake_write_count;

/* --- lifecycle ------------------------------------------------- */

void fw_reset(void)
{
    fake_flash_init();
    fake_uart_init();
    crc32_init();
    protocol_init();
}

/* Restart the protocol layer WITHOUT clearing flash, which is what a
   reboot actually does. Power-cut and rollback scenarios depend on
   this distinction. */
void fw_reboot(void)
{
    fake_uart_init();
    crc32_init();
    protocol_init();
}

/* --- driving the state machine --------------------------------- */

void fw_feed(const uint8_t *data, uint32_t len) { fake_uart_push_rx(data, len); }
void fw_poll(uint32_t now_ms)                   { protocol_poll(now_ms); }
uint32_t fw_take(uint8_t *out, uint32_t max)    { return fake_uart_pop_tx(out, max); }

/* --- observation ----------------------------------------------- */

int      fw_state(void)            { return (int)protocol_get_state(); }
int      fw_update_complete(void)  { return protocol_update_complete(); }
uint32_t fw_frames_received(void)  { return protocol_frames_received(); }
uint32_t fw_frames_rejected(void)  { return protocol_frames_rejected(); }
uint32_t fw_bytes_written(void)    { return protocol_bytes_written(); }
unsigned fw_erase_count(void)      { return fake_erase_count; }
unsigned fw_write_count(void)      { return fake_write_count; }

/* Raw flash access for assertions. */
void fw_read_flash(uint32_t address, uint8_t *out, uint32_t len)
{
    memcpy(out, (const void *)(uintptr_t)address, len);
}

void fw_write_flash_raw(uint32_t address, const uint8_t *data, uint32_t len)
{
    memcpy((void *)(uintptr_t)address, data, len);
}

/* --- metadata, through the real manager ------------------------ */

int fw_metadata_read(uint8_t *out48)
{
    metadata_t m;
    if (!metadata_read(&m)) return 0;
    memcpy(out48, &m, sizeof(m));
    return 1;
}

int fw_metadata_write(const uint8_t *in48)
{
    metadata_t m;
    memcpy(&m, in48, sizeof(m));
    return (int)metadata_write(&m);
}

int fw_metadata_erase_all(void) { return (int)metadata_erase_all(); }

uint32_t fw_metadata_size(void) { return (uint32_t)sizeof(metadata_t); }
