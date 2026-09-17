/*
 * Host stand-ins for the three hardware drivers.
 *
 * What this replaces, and why
 * ---------------------------
 * flash.c, crc.c and uart.c talk to STM32 registers, so they cannot
 * run on a PC. They are replaced here by models of the CONTRACT their
 * headers document -- not by simplified versions of their behaviour.
 * The flash model rejects misaligned writes and writes to
 * non-erased memory exactly as the controller does, because those are
 * the rules the code under test depends on.
 *
 * What is NOT replaced is the code actually being tested:
 * metadata_mgr.c and protocol_mgr.c are compiled from drivers/ and
 * bootloader/src/ unmodified. No #ifdef, no test-only branch. If a
 * test passes here it passed against the same source that runs on
 * silicon.
 *
 * The flash array is mapped at 0x08000000
 * --------------------------------------
 * The firmware dereferences flash addresses directly -- for instance
 * crc32_compute((const uint8_t *)SLOT_ADDR(slot), size) in
 * on_end_update(). Redirecting that through an accessor would mean
 * editing the code under test, which defeats the purpose. Instead the
 * harness mmaps one megabyte at the real base address, so the
 * pointers the firmware forms are valid here for the same reason they
 * are valid on the chip.
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>

#include "flash.h"
#include "crc.h"
#include "uart.h"
#include "metadata.h"

/* ----------------------------------------------------------------
 * Flash
 * ---------------------------------------------------------------- */

static uint8_t *flash_mem;      /* mapped at FLASH_BASE_ADDR */

/* Counters the tests assert on. */
unsigned fake_erase_count;
unsigned fake_write_count;

void fake_flash_init(void)
{
    if (!flash_mem) {
        void *p = mmap((void *)(uintptr_t)FLASH_BASE_ADDR, FLASH_TOTAL_SIZE,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (p == MAP_FAILED) {
            fprintf(stderr, "fake flash: cannot map 1 MB at 0x%08lX\n",
                    (unsigned long)FLASH_BASE_ADDR);
            return;
        }
        flash_mem = (uint8_t *)p;
    }
    memset(flash_mem, 0xFF, FLASH_TOTAL_SIZE);
    fake_erase_count = 0;
    fake_write_count = 0;
}

static int in_range(uint32_t address, uint32_t len)
{
    if (address < FLASH_BASE_ADDR) return 0;
    if (address + len > FLASH_BASE_ADDR + FLASH_TOTAL_SIZE) return 0;
    if (address + len < address) return 0;
    return 1;
}

flash_status_t flash_erase_page(uint32_t address)
{
    if (!in_range(address, FLASH_PAGE_SIZE)) return FLASH_ERR_RANGE;
    if (address % FLASH_PAGE_SIZE)          return FLASH_ERR_ALIGN;

    memset(flash_mem + (address - FLASH_BASE_ADDR), 0xFF, FLASH_PAGE_SIZE);
    fake_erase_count++;
    return FLASH_OK;
}

flash_status_t flash_write(uint32_t address, const uint8_t *data, uint32_t len)
{
    if (data == 0 || len == 0U)          return FLASH_ERR_ALIGN;
    if (address % 8U || len % 8U)        return FLASH_ERR_ALIGN;
    if (!in_range(address, len))         return FLASH_ERR_RANGE;

    uint8_t *dst = flash_mem + (address - FLASH_BASE_ADDR);

    /* The controller raises PROGERR on a double-word that is not
       erased. Modelled, because callers rely on erase-before-write
       being enforced rather than merely expected. */
    for (uint32_t i = 0; i < len; i++) {
        if (dst[i] != 0xFF) return FLASH_ERR_PROG;
    }

    memcpy(dst, data, len);
    fake_write_count++;

    /* flash_write() verifies by read-back on the target; do the same
       here so a broken model cannot pass silently. */
    if (memcmp(dst, data, len) != 0) return FLASH_ERR_VERIFY;
    return FLASH_OK;
}

int flash_is_erased(uint32_t address, uint32_t len)
{
    if (!in_range(address, len)) return 0;
    const uint8_t *p = flash_mem + (address - FLASH_BASE_ADDR);
    for (uint32_t i = 0; i < len; i++) {
        if (p[i] != 0xFF) return 0;
    }
    return 1;
}

/* ----------------------------------------------------------------
 * CRC -- software model of the peripheral
 *
 * POLY 0x04C11DB7, INIT 0xFFFFFFFF, REV_IN = 01 (per byte), REV_OUT = 0,
 * no final XOR. Must agree with tools/crc32.py and with silicon:
 * "123456789" -> 0x9B63D02C.
 * ---------------------------------------------------------------- */

static uint32_t crc_reg;

static uint8_t reverse_byte(uint8_t b)
{
    b = (uint8_t)(((b & 0xF0) >> 4) | ((b & 0x0F) << 4));
    b = (uint8_t)(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
    b = (uint8_t)(((b & 0xAA) >> 1) | ((b & 0x55) << 1));
    return b;
}

void crc32_init(void)  { crc_reg = 0xFFFFFFFFUL; }
void crc32_reset(void) { crc_reg = 0xFFFFFFFFUL; }

void crc32_update(const uint8_t *data, uint32_t len)
{
    while (len--) {
        crc_reg ^= (uint32_t)reverse_byte(*data++) << 24;
        for (int i = 0; i < 8; i++) {
            crc_reg = (crc_reg & 0x80000000UL)
                    ? ((crc_reg << 1) ^ 0x04C11DB7UL)
                    : (crc_reg << 1);
        }
    }
}

uint32_t crc32_get(void) { return crc_reg; }

uint32_t crc32_compute(const uint8_t *data, uint32_t len)
{
    crc32_reset();
    crc32_update(data, len);
    return crc32_get();
}

/* ----------------------------------------------------------------
 * UART -- two byte queues
 *
 * protocol_mgr.c only ever uses uart_getc, uart_write and uart_flush
 * on uart_proto. The debug instance exists so the symbol resolves.
 * ---------------------------------------------------------------- */

uart_t uart_debug;
uart_t uart_proto;

#define Q_SIZE  8192
static uint8_t  rx_q[Q_SIZE]; static uint32_t rx_head, rx_tail;
static uint8_t  tx_q[Q_SIZE]; static uint32_t tx_head, tx_tail;

void fake_uart_init(void) { rx_head = rx_tail = tx_head = tx_tail = 0; }

/* Injected by the test: bytes the board is about to receive. */
void fake_uart_push_rx(const uint8_t *data, uint32_t len)
{
    while (len-- && rx_head < Q_SIZE) rx_q[rx_head++] = *data++;
}

/* Collected by the test: bytes the board transmitted. */
uint32_t fake_uart_pop_tx(uint8_t *out, uint32_t max)
{
    uint32_t n = 0;
    while (tx_tail < tx_head && n < max) out[n++] = tx_q[tx_tail++];
    if (tx_tail == tx_head) tx_tail = tx_head = 0;
    return n;
}

int uart_getc(uart_t *u, uint8_t *out)
{
    (void)u;
    if (rx_tail >= rx_head) { rx_tail = rx_head = 0; return 0; }
    *out = rx_q[rx_tail++];
    return 1;
}

void uart_write(uart_t *u, const uint8_t *data, uint32_t len)
{
    (void)u;
    while (len-- && tx_head < Q_SIZE) tx_q[tx_head++] = *data++;
}

void uart_putc(uart_t *u, char c)        { uart_write(u, (const uint8_t *)&c, 1); }
void uart_puts(uart_t *u, const char *s) { if (s) uart_write(u, (const uint8_t *)s, (uint32_t)strlen(s)); }
void uart_flush(uart_t *u)               { (void)u; }

uint32_t uart_available(uart_t *u)       { (void)u; return rx_head - rx_tail; }
void     uart_rx_flush(uart_t *u)        { (void)u; rx_head = rx_tail = 0; }
uint32_t uart_read(uart_t *u, uint8_t *d, uint32_t max)
{
    uint32_t n = 0; uint8_t b;
    while (n < max && uart_getc(u, &b)) d[n++] = b;
    return n;
}

void uart_hex32(uart_t *u, uint32_t v) { (void)u; (void)v; }
void uart_hex8 (uart_t *u, uint8_t v)  { (void)u; (void)v; }
void uart_dec  (uart_t *u, uint32_t v) { (void)u; (void)v; }

uint32_t uart_overrun_count(uart_t *u)       { (void)u; return 0; }
uint32_t uart_hw_overrun_count(uart_t *u)    { (void)u; return 0; }
uint32_t uart_framing_error_count(uart_t *u) { (void)u; return 0; }
uint32_t uart_noise_error_count(uart_t *u)   { (void)u; return 0; }
void     uart_reset_counters(uart_t *u)      { (void)u; }
void     uart_debug_init(uint32_t b)         { (void)b; }
void     uart_proto_init(uint32_t b)         { (void)b; }
