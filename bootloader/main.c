#include <stdint.h>

#include "uart.h"
#include "crc.h"
#include "flash.h"
#include "systick.h"
#include "iwdg.h"
#include "metadata.h"
#include "metadata_mgr.h"
#include "protocol.h"
#include "protocol_mgr.h"
#include "boot_request.h"
#include "board.h"
#include "fault.h"

/*
 * Bootloader -- entry point.
 *
 * Boot sequence
 * -------------
 *   1. Initialise peripherals
 *   2. Read metadata
 *   3. Decide: jump to the application, or wait for an update
 *   4. If an update is requested, handle it then restart
 *
 * Step 3 depends on the recorded state:
 *
 *   No metadata     -> update mode: nothing to run
 *   EMPTY           -> update mode
 *   IN_PROGRESS     -> update mode: a transfer was interrupted and
 *                      the target slot holds a partial image
 *   VALID           -> jump immediately
 *   TESTING         -> jump, after incrementing the failure counter.
 *                      Past the threshold, roll back to the other slot.
 *
 * Boot request
 * ------------
 * Before jumping, the bootloader listens on the serial link for two
 * seconds, so that a host can take over a board that is otherwise
 * about to run. That window is short by design -- it costs two
 * seconds on every boot -- and hitting it means resetting the board
 * and racing it.
 *
 * The boot request flag removes the race without lengthening the
 * default window. A host with a debugger, or the application itself,
 * writes a magic word to the last RAM word and resets; RAM survives
 * a system reset, so the bootloader finds it and stretches the same
 * listen window to thirty seconds. See shared/boot_request.h.
 *
 * Deliberately, the flag does not change WHAT the bootloader does,
 * only how long it waits. The boot decision below runs unchanged, so
 * an update that never arrives costs thirty seconds and then the
 * board boots exactly as it would have.
 *
 * The TESTING mechanism
 * ---------------------
 * A correct CRC proves an image's integrity, not its correctness: a
 * firmware transmitted without a single corrupted bit can still
 * crash within its first second.
 *
 * The bootloader therefore jumps to a TESTING image after
 * incrementing the failure counter. If the application starts
 * correctly it must clear that counter and set the state to VALID.
 * If it crashes, the watchdog forces a reset with the counter
 * untouched; past the threshold the bootloader switches back.
 *
 * The application thus carries a responsibility. Without its
 * confirmation, no rollback is possible.
 *
 * Watchdog
 * --------
 * The IWDG is started on the very first line of main(), before any
 * other initialisation.
 *
 * Starting it just before the jump would have covered the
 * application but left the bootloader itself unwatched -- and it
 * contains several blocking waits (USART TXE, flash BSY, the receive
 * loop), any of which can hang if a peripheral stops responding. The
 * board would then freeze without the watchdog ever being armed.
 *
 * It is the same reasoning that forbids starting it from the
 * application's main(): surveillance must begin before the thing it
 * watches.
 *
 * In exchange, the bootloader must refresh the watchdog itself while
 * in update mode. The cost is nil: the loop runs thousands of times
 * per second, and the longest blocking operation -- a page erase at
 * roughly twenty milliseconds -- stays a hundred and fifty times
 * below the three-second timeout.
 */

#define BOOT_WAIT_MS        2000U    /* listen window after reset      */
#define OTA_WAIT_MS         30000U   /* listen window after a request  */

#define SCB_VTOR            (*(volatile uint32_t *)0xE000ED08UL)

/* RCC registers, needed to return peripherals close to their reset
   state before handing over. */
#define RCC_BASE            0x40021000UL
#define RCC_AHB1ENR         (*(volatile uint32_t *)(RCC_BASE + 0x48))
#define RCC_AHB2ENR         (*(volatile uint32_t *)(RCC_BASE + 0x4C))
#define RCC_APB1ENR1        (*(volatile uint32_t *)(RCC_BASE + 0x58))
#define RCC_APB2ENR         (*(volatile uint32_t *)(RCC_BASE + 0x60))

#define NVIC_ICER0          (*(volatile uint32_t *)0xE000E180UL)
#define NVIC_ICER1          (*(volatile uint32_t *)0xE000E184UL)
#define NVIC_ICPR0          (*(volatile uint32_t *)0xE000E280UL)
#define NVIC_ICPR1          (*(volatile uint32_t *)0xE000E284UL)


/* Fault reporting writes through the blocking TX path deliberately:
   the interrupt-driven receive side may be exactly what failed. */
void fault_putc(char c)
{
    uart_putc(&uart_debug, c);
}


/* ----------------------------------------------------------------
 * Logging
 * ---------------------------------------------------------------- */

static void log_state(uint8_t s)
{
    switch (s) {
    case STATE_EMPTY:       uart_puts(&uart_debug, "EMPTY");       break;
    case STATE_IN_PROGRESS: uart_puts(&uart_debug, "IN_PROGRESS"); break;
    case STATE_TESTING:     uart_puts(&uart_debug, "TESTING");     break;
    case STATE_VALID:       uart_puts(&uart_debug, "VALID");       break;
    default:                uart_hex8(&uart_debug, s);             break;
    }
}


/* ----------------------------------------------------------------
 * Jump to the application
 * ---------------------------------------------------------------- */

static int slot_looks_bootable(uint32_t base)
{
    uint32_t sp    = *(volatile uint32_t *)base;
    uint32_t reset = *(volatile uint32_t *)(base + 4U);

    /* The first word is the initial stack pointer: it must point
       into RAM. The second is the reset handler address, which must
       fall inside flash.

       A missing image leaves 0xFFFFFFFF in both, which these checks
       reject. Jumping to a non-existent image would raise a silent
       HardFault with no message at all. */
    if ((sp & 0xFFF00000UL) != 0x20000000UL) {
        return 0;
    }
    if (reset < FLASH_BASE_ADDR ||
        reset >= (FLASH_BASE_ADDR + FLASH_TOTAL_SIZE)) {
        return 0;
    }

    /* Bit 0 must be set: Cortex-M cores only execute Thumb, and this
       bit tells the processor so. An even address would raise an
       immediate HardFault. */
    if ((reset & 1U) == 0U) {
        return 0;
    }

    return 1;
}


static void jump_to_application(uint32_t base)
{
    uint32_t app_sp    = *(volatile uint32_t *)base;
    uint32_t app_reset = *(volatile uint32_t *)(base + 4U);

    uart_puts(&uart_debug, "\r\nJumping to ");
    uart_hex32(&uart_debug, base);
    uart_puts(&uart_debug, "\r\n\r\n");
    uart_flush(&uart_debug);

    /* Final refresh: the application then gets the full timeout to
       initialise and reach its own first refresh. */
    iwdg_feed();

    __asm__ volatile ("cpsid i");

    /* Return peripherals close to their reset state. The application
       will reconfigure them, but it assumes they start clean: an
       already-enabled USART or a still-armed interrupt would produce
       behaviour that looks inexplicable.

       The watchdog is the exception: it cannot be stopped, and that
       is precisely the intent. The application inherits it and must
       refresh it, or be reset -- which is what triggers rollback. */
    systick_deinit();

    NVIC_ICER0 = 0xFFFFFFFFUL;      /* disable every IRQ */
    NVIC_ICER1 = 0xFFFFFFFFUL;
    NVIC_ICPR0 = 0xFFFFFFFFUL;      /* clear pending ones */
    NVIC_ICPR1 = 0xFFFFFFFFUL;

    RCC_APB1ENR1 &= ~(1U << 17);    /* USART2 -- debug console  */
    RCC_APB2ENR  &= ~(1U << 14);    /* USART1 -- protocol link   */
    RCC_AHB1ENR  &= ~(1U << 12);    /* CRC                       */

    /* Relocate the vector table. Without this the application starts,
       then crashes on its first interrupt: the processor would look
       for the handler in the bootloader's table. */
    SCB_VTOR = base;

    __asm__ volatile ("dsb");
    __asm__ volatile ("isb");

    /* Load the application's stack pointer before jumping. The
       bootloader's own has no meaning on the other side. */
    __asm__ volatile ("msr msp, %0" : : "r" (app_sp) : );

    __asm__ volatile ("cpsie i");

    /* The jump itself. This never returns. */
    ((void (*)(void))app_reset)();

    /* Unreachable */
    while (1) {
    }
}


/* ----------------------------------------------------------------
 * Verifying an image before running it
 * ---------------------------------------------------------------- */

static int image_is_intact(const metadata_t *meta, uint8_t slot)
{
    const slot_info_t *info = &meta->slot[slot];

    if (info->size == 0U || info->size > SLOT_SIZE) {
        return 0;
    }

    uint32_t base = SLOT_ADDR(slot);

    if (!slot_looks_bootable(base)) {
        return 0;
    }

    /* CRC recomputed on every boot. A flash cell can degrade over
       time; better to find out here than to execute corrupted code. */
    uint32_t computed = crc32_compute((const uint8_t *)base, info->size);

    return (computed == info->crc32);
}


/* ----------------------------------------------------------------
 * Update mode
 * ---------------------------------------------------------------- */

static void update_mode(uint32_t limit_ms)
{
    uart_puts(&uart_debug, "Update mode");
    if (limit_ms > 0U) {
        uart_puts(&uart_debug, " (");
        uart_dec(&uart_debug, limit_ms);
        uart_puts(&uart_debug, " ms)");
    }
    uart_puts(&uart_debug, "\r\n");

    uint32_t start = millis();

    while (1) {
        /* Unconditional refresh here: the bootloader has no notion of
           a healthy state to check, it simply handles what arrives.
           Conditioning the refresh is the application's job. */
        iwdg_feed();

        uint32_t now = millis();

        protocol_poll(now);

        if (protocol_update_complete()) {
            uart_puts(&uart_debug, "\r\nUpdate complete, restarting\r\n");
            uart_flush(&uart_debug);
            delay_ms(50);

            /* Software reset via AIRCR. Starting over guarantees a
               clean state rather than jumping from a bootloader whose
               peripherals are already configured. */
            *(volatile uint32_t *)0xE000ED0CUL = 0x05FA0004UL;

            while (1) {
            }
        }

        /* The listen window only expires if nothing has started: a
           transfer in progress must not be cut short by the boot
           timeout. */
        if (limit_ms > 0U &&
            protocol_get_state() == PROTO_IDLE &&
            (now - start) > limit_ms) {
            return;
        }
    }
}


/* ----------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------- */

int main(void)
{
    /* Before anything else: from here on, any hang -- in the
       bootloader or in the application -- causes a reset. */
#ifdef DEBUG_BUILD
    /* Debug builds only.
     *
     * shared/iwdg.h states the rule plainly: freezing the counter under
     * the debugger is a development convenience and must not ship,
     * because it also masks a genuine hang reached through the debug
     * interface. Calling it unconditionally -- which this code did --
     * broke that rule in the same repository that documents it.
     *
     * Enable with:  make DEBUG=1 */
    iwdg_freeze_on_debug();
#endif
    iwdg_start(IWDG_TIMEOUT_MS);

    /* The last reset cause is read before being cleared: it tells a
       crashed application apart from a deliberate button press. */
    int reset_by_watchdog = iwdg_caused_last_reset();
    iwdg_clear_reset_flags();

    systick_init(SYSTEM_CLOCK_HZ);
    uart_debug_init(UART_BAUD);
    uart_proto_init(UART_BAUD);
    crc32_init();
    protocol_init();

    /* ---- Boot request ------------------------------------------------
     * The flag is read here and cleared immediately, before anything
     * is done with it: clearing afterwards would mean a reset during
     * the transfer left the request set, and the board would re-enter
     * update mode on every boot from then on.
     *
     * What the flag changes is how LONG the listen window is, not
     * whether the application eventually runs. Waiting indefinitely
     * was the obvious first version and the wrong one: a transfer
     * that never starts -- wrong file, busy port, host interrupted --
     * would park a board that has a perfectly good application in
     * flash, recoverable only with the reset button. That is exactly
     * the button this mechanism exists to avoid needing.
     *
     * So the window is long enough that no plausible host is late,
     * and finite so that a failed attempt leaves the board where it
     * started. update_mode() only honours its timeout while the
     * protocol is idle, so a transfer already under way is never cut
     * short by it.
     * ---------------------------------------------------------------- */
    uint32_t listen_ms   = BOOT_WAIT_MS;
    int      ota_request = 0;

    if (boot_request_pending()) {
        boot_request_clear();
        ota_request = 1;
        listen_ms   = OTA_WAIT_MS;
    }

    uart_puts(&uart_debug, "\r\n\r\n========================================\r\n");
    uart_puts(&uart_debug, "  BOOTLOADER v");
    uart_dec(&uart_debug, (BOOTLOADER_VERSION >> 16) & 0xFFU);
    uart_putc(&uart_debug, '.');
    uart_dec(&uart_debug, (BOOTLOADER_VERSION >> 8) & 0xFFU);
    uart_putc(&uart_debug, '.');
    uart_dec(&uart_debug, BOOTLOADER_VERSION & 0xFFU);
    uart_puts(&uart_debug, "\r\n========================================\r\n");

    if (reset_by_watchdog) {
        uart_puts(&uart_debug, "Reset caused by the watchdog\r\n");
    }

    if (ota_request) {
        /* The host script waits for this line before starting the
           transfer. Not seeing it means the flag was ignored -- an
           older bootloader, whose stack still reaches the flag word. */
        uart_puts(&uart_debug, "OTA request accepted, listening for ");
        uart_dec(&uart_debug, OTA_WAIT_MS / 1000U);
        uart_puts(&uart_debug, " s\r\n");
    }

    metadata_t meta;

    if (!metadata_read(&meta)) {
        uart_puts(&uart_debug, "No valid metadata\r\n");
        update_mode(0);             /* wait indefinitely */
        while (1) { }
    }

    uint8_t active = meta.active_slot;

    uart_puts(&uart_debug, "Active slot : ");
    uart_putc(&uart_debug, (char)('A' + active));
    uart_puts(&uart_debug, "\r\nState       : ");
    log_state(meta.slot[active].state);
    uart_puts(&uart_debug, "\r\nSize        : ");
    uart_dec(&uart_debug, meta.slot[active].size);
    uart_puts(&uart_debug, " bytes\r\nVersion     : ");
    uart_hex32(&uart_debug, meta.slot[active].version);
    uart_puts(&uart_debug, "\r\nBoot fails  : ");
    uart_dec(&uart_debug, meta.boot_fail_count);
    uart_puts(&uart_debug, "\r\n");

    /* The other slot is shown too: it is the fallback in case of
       rollback, and knowing what it holds avoids discovering too late
       that it is empty. */
    uint8_t other_slot = OTHER_SLOT(active);
    uart_puts(&uart_debug, "Fallback    : slot ");
    uart_putc(&uart_debug, (char)('A' + other_slot));
    uart_puts(&uart_debug, ", ");
    log_state(meta.slot[other_slot].state);
    uart_puts(&uart_debug, ", ");
    uart_dec(&uart_debug, meta.slot[other_slot].size);
    uart_puts(&uart_debug, " bytes\r\n\r\n");

    switch (meta.slot[active].state) {

    case STATE_VALID:
        if (image_is_intact(&meta, active)) {
            update_mode(listen_ms);      /* give the host a chance */
            jump_to_application(SLOT_ADDR(active));
        }
        uart_puts(&uart_debug, "Image invalid despite VALID state\r\n");
        break;

    case STATE_TESTING:
        if (meta.boot_fail_count >= MAX_BOOT_FAILURES) {
            /* The application never confirmed itself. Fall back to
               the previous slot. */
            uart_puts(&uart_debug, "Failure threshold reached, rolling back\r\n");

            uint8_t other = OTHER_SLOT(active);

            /* Check the fallback BEFORE switching: rejecting the
               current image only to find nothing behind it would be
               worse than continuing to retry. */
            if (meta.slot[other].state == STATE_EMPTY ||
                !image_is_intact(&meta, other)) {
                uart_puts(&uart_debug, "No usable fallback image\r\n");
                break;
            }

            /* Only active_slot, the counter and the rejected slot's
               state change. Both image descriptions stay intact --
               which is the whole point of keeping them separate. */
            metadata_t previous = meta;
            previous.slot[active].state = STATE_EMPTY;
            previous.active_slot       = other;
            previous.boot_fail_count   = 0;

            if (metadata_write(&previous) == META_OK &&
                slot_looks_bootable(SLOT_ADDR(other))) {
                uart_puts(&uart_debug, "Falling back to slot ");
                uart_putc(&uart_debug, (char)('A' + other));
                uart_puts(&uart_debug, "\r\n");
                jump_to_application(SLOT_ADDR(other));
            }
            uart_puts(&uart_debug, "No usable fallback image\r\n");
            break;
        }

        if (image_is_intact(&meta, active)) {
            /* Increment BEFORE jumping. If the application crashes,
               the counter has already advanced by the next reset.
               Incrementing afterwards would have no effect, since the
               jump never returns. */
            metadata_t attempt = meta;
            attempt.boot_fail_count = (uint8_t)(meta.boot_fail_count + 1U);
            metadata_write(&attempt);

            uart_puts(&uart_debug, "Attempt ");
            uart_dec(&uart_debug, attempt.boot_fail_count);
            uart_putc(&uart_debug, '/');
            uart_dec(&uart_debug, MAX_BOOT_FAILURES);
            uart_puts(&uart_debug, "\r\n");

            update_mode(listen_ms);
            jump_to_application(SLOT_ADDR(active));
        }
        uart_puts(&uart_debug, "Image under test is invalid\r\n");
        break;

    case STATE_IN_PROGRESS:
        uart_puts(&uart_debug, "Interrupted transfer detected\r\n");
        break;

    case STATE_EMPTY:
    default:
        uart_puts(&uart_debug, "No firmware installed\r\n");
        break;
    }

    /* Every path that did not jump ends here: there is nothing
       runnable, so wait indefinitely for an update. */
    update_mode(0);

    while (1) {
    }
}
