#include <stdint.h>

#include "crc.h"
#include "flash.h"
#include "metadata.h"
#include "metadata_mgr.h"
#include "iwdg.h"
#include "boot_request.h"
#include "board.h"
#include "fault.h"

/*
 * Application -- demonstrates the update lifecycle.
 *
 * Self-confirmation
 * -----------------
 * The bootloader jumps to a TESTING image after incrementing its
 * failure counter. If the application does nothing, that counter
 * reaches the threshold after a few restarts and the bootloader
 * switches back to the previous slot.
 *
 * That is the rollback mechanism itself: a correct CRC proves the
 * image's integrity, not its correctness. A firmware transmitted
 * without a single corrupted bit can still crash within its first
 * second.
 *
 * The application must therefore confirm its own start-up: set the
 * state to VALID and clear the counter. Without that confirmation no
 * rollback is possible -- but without it, no firmware survives past
 * three boots either.
 *
 * When to confirm
 * ---------------
 * Confirming on the first line of main() would empty the mechanism
 * of meaning: an application that crashes moments later would still
 * have been declared healthy.
 *
 * What counts as a successful start depends on the product. Here a
 * few complete application cycles are required. A real system would
 * confirm after checking its peripherals, establishing a link, or
 * meeting whatever criterion is meaningful for it.
 *
 * Inherited watchdog
 * ------------------
 * The bootloader started the IWDG before jumping, and it can no
 * longer be stopped. This application must therefore refresh it, or
 * it will be reset after three seconds -- which advances the failure
 * counter and, after three attempts, triggers rollback.
 *
 * That is the mechanism working as intended: an application that
 * hangs has no way to keep refreshing, so its failure is detected
 * without it having to report anything.
 *
 * Refreshing here is conditional on the application cycle having
 * advanced. An unconditional refresh would only catch a complete
 * hang: a program looping over a section that happens to contain the
 * call would keep the watchdog quiet while doing nothing useful.
 *
 * OTA trigger
 * -----------
 * The main loop also watches the serial link for a request to hand
 * control back to the bootloader, so that an update needs neither the
 * reset button nor a debugger. The application sets the boot request
 * flag (shared/boot_request.h) and resets itself; the bootloader finds
 * the flag and stretches its listen window.
 *
 * The confirmation this trigger requires, and why it is a streak of
 * polls rather than a magic word, is argued where it is implemented.
 */

#define RCC_BASE            0x40021000UL
#define GPIOA_BASE          0x48000000UL
#define USART2_BASE         0x40004400UL

#define RCC_AHB2ENR         (*(volatile uint32_t *)(RCC_BASE + 0x4C))
#define RCC_APB1ENR1        (*(volatile uint32_t *)(RCC_BASE + 0x58))
#define RCC_APB2ENR         (*(volatile uint32_t *)(RCC_BASE + 0x60))

#define GPIOA_MODER         (*(volatile uint32_t *)(GPIOA_BASE + 0x00))
#define GPIOA_ODR           (*(volatile uint32_t *)(GPIOA_BASE + 0x14))
#define GPIOA_AFRL          (*(volatile uint32_t *)(GPIOA_BASE + 0x20))
#define GPIOA_AFRH          (*(volatile uint32_t *)(GPIOA_BASE + 0x24))

#define USART1_BASE         0x40013800UL
#define USART1_CR1          (*(volatile uint32_t *)(USART1_BASE + 0x00))
#define USART1_BRR          (*(volatile uint32_t *)(USART1_BASE + 0x0C))
#define USART1_ISR          (*(volatile uint32_t *)(USART1_BASE + 0x1C))
#define USART1_ICR          (*(volatile uint32_t *)(USART1_BASE + 0x20))
#define USART1_RDR          (*(volatile uint32_t *)(USART1_BASE + 0x24))

#define USART2_CR1          (*(volatile uint32_t *)(USART2_BASE + 0x00))
#define USART2_BRR          (*(volatile uint32_t *)(USART2_BASE + 0x0C))
#define USART2_ISR          (*(volatile uint32_t *)(USART2_BASE + 0x1C))
#define USART2_ICR          (*(volatile uint32_t *)(USART2_BASE + 0x20))
#define USART2_RDR          (*(volatile uint32_t *)(USART2_BASE + 0x24))
#define USART2_TDR          (*(volatile uint32_t *)(USART2_BASE + 0x28))

#define SCB_VTOR            (*(volatile uint32_t *)0xE000ED08UL)

#define LED_PIN             5



/* Application cycles required before declaring the image healthy. */
#define CYCLES_BEFORE_CONFIRMATION   3

/* OTA trigger: the character the host holds the line down with, and
   the number of consecutive polls that must see nothing else before
   the request is acted on. */
#define OTA_TRIGGER_BYTE            'U'
#define OTA_CONFIRM_POLLS           4

/* Units of work one main-loop pass owes before the watchdog is
   refreshed. See the accumulator in main() for why each is derived
   from observed hardware state rather than from reaching a line. */
#define WORK_BLINK                  (1U << 0)
#define WORK_REPORT                 (1U << 1)
#define WORK_LINK                   (1U << 2)
#define WORK_ALL                    (WORK_BLINK | WORK_REPORT | WORK_LINK)


/* ----------------------------------------------------------------
 * Minimal UART
 *
 * Transmit is blocking, receive is polled with no buffer. The
 * bootloader has a real interrupt-driven driver with a ring buffer
 * (bootloader/src/uart.c); the application deliberately does not,
 * because the only thing it ever receives is the OTA trigger below,
 * which is designed to survive a receive path this thin.
 * ---------------------------------------------------------------- */

static void uart_init(void)
{
    RCC_AHB2ENR  |= (1U << 0);
    RCC_APB1ENR1 |= (1U << 17);  /* USART2 clock */
    RCC_APB2ENR  |= (1U << 14);  /* USART1 clock */

    /* Configure PA2/PA3 for USART2 (debug) and PA9/PA10 for USART1 (ESP32) */
    GPIOA_MODER &= ~((3U << 4) | (3U << 6) | (3U << 18) | (3U << 20));
    GPIOA_MODER |=  ((2U << 4) | (2U << 6) | (2U << 18) | (2U << 20));
    GPIOA_AFRL  &= ~((0xFU << 8) | (0xFU << 12));
    GPIOA_AFRL  |=  ((7U << 8)   | (7U << 12));
    GPIOA_AFRH  &= ~((0xFU << 4) | (0xFU << 8));
    GPIOA_AFRH  |=  ((7U << 4)   | (7U << 8));

    /* Both peripherals are disabled before BRR is written.
     *
     * The bootloader hands over with UE still set -- gating a clock
     * preserves register contents, and USART1's clock was not gated at
     * all until recently. RM0351 40.5.4 warns that the baud counters
     * reload on a BRR write, so the rate must not be changed while the
     * peripheral is running. Writing BRR first, as this did, risks the
     * new divisor never taking effect and the application silently
     * inheriting the bootloader's. */
    USART2_CR1 = 0;
    USART1_CR1 = 0;

    /* USART2 - Debug logs */
    USART2_BRR = USART_BRR_OVER16(SYSTEM_CLOCK_HZ, UART_BAUD);
    USART2_CR1 = (1U << 3) | (1U << 2) | (1U << 0);   /* TE | RE | UE */
    USART2_ICR = (1U << 0) | (1U << 1) | (1U << 2) | (1U << 3);
    (void)USART2_RDR;

    /* USART1 - ESP32 OTA trigger */
    USART1_BRR = USART_BRR_OVER16(SYSTEM_CLOCK_HZ, UART_BAUD);
    USART1_CR1 = (1U << 3) | (1U << 2) | (1U << 0);   /* TE | RE | UE */
    USART1_ICR = (1U << 0) | (1U << 1) | (1U << 2) | (1U << 3);
    (void)USART1_RDR;
}

static void uart_putc(char c)
{
    while (!(USART2_ISR & (1U << 7))) { }
    USART2_TDR = (uint32_t)(uint8_t)c;
}

static void uart_puts(const char *s)
{
    while (*s) {
        uart_putc(*s++);
    }
}

static void uart_hex32(uint32_t v)
{
    const char *d = "0123456789ABCDEF";
    uart_puts("0x");
    for (int i = 28; i >= 0; i -= 4) {
        uart_putc(d[(v >> i) & 0xFU]);
    }
}

static void uart_dec(uint32_t v)
{
    char tmp[10];
    int n = 0;
    if (v == 0U) {
        uart_putc('0');
        return;
    }
    while (v > 0U) {
        tmp[n++] = (char)('0' + (v % 10U));
        v /= 10U;
    }
    while (n-- > 0) {
        uart_putc(tmp[n]);
    }
}

/* drivers/fault.c reports through this. The application's TX path is
   already blocking and register-level, which is what a fault context
   requires. */
void fault_putc(char c)
{
    uart_putc(c);
}

/*
 * Crude blink delay, deliberately not the SysTick time base.
 *
 * shared/systick.h criticises nop loops because their duration depends
 * on the optimisation level and the clock -- which is correct wherever
 * a duration is being MEASURED. Nothing is measured here: this only
 * has to be long enough to see the LED and short enough that the main
 * loop refreshes the watchdog comfortably inside three seconds. At
 * 4 MHz it lands near half a second, two orders of magnitude of
 * margin, and volatile keeps the loop from being optimised away.
 *
 * Pulling systick.c into the application to time a blink would add an
 * interrupt, a driver and a vector for no accuracy that matters. The
 * trade is deliberate; it is recorded here so the contradiction with
 * systick.h does not read as an oversight.
 */
static void delay(volatile uint32_t n)
{
    while (n--) {
        __asm__("nop");
    }
}

/* Non-blocking receive. Returns 1 and stores the byte if one is
 * available, 0 otherwise.
 *
 * ORE has to be cleared explicitly. Until it is, the USART stops
 * moving new bytes into RDR, so a receive path that ignores it goes
 * deaf after the first overrun with nothing visible to show for it --
 * the same point bootloader/src/uart.c makes at more length in its
 * interrupt handler. This is the polled version of that rule.
 *
 * Overrun is not an edge case here. This is polled once per blink,
 * and at 115200 baud a byte lands every 87 us, so anything the host
 * sends faster than one byte per poll overruns by construction. The
 * trigger below is built to tolerate that rather than to pretend it
 * does not happen. */
static int uart_getc(uint8_t *c)
{
    if (USART1_ISR & (1U << 3)) {       /* ORE */
        USART1_ICR = (1U << 3);
    }

    if (USART1_ISR & (1U << 5)) {       /* RXNE */
        *c = (uint8_t)(USART1_RDR & 0xFFU);
        return 1;
    }

    return 0;
}

/* Software reset via AIRCR -- identical to what the bootloader uses.
 * The CPU restarts from the reset vector; RAM is preserved. */
static void software_reset(void)
{
    /* Wait for the shift register to empty before pulling the rug
       out. TXE only reports that the holding register is free; TC
       reports that the last bit is actually on the wire. Resetting on
       TXE cuts the final character in half. */
    while (!(USART2_ISR & (1U << 6))) { }    /* TC */

    *(volatile uint32_t *)0xE000ED0CUL = 0x05FA0004UL;

    while (1) { }    /* the reset takes a few cycles to land */
}


/* ----------------------------------------------------------------
 * OTA trigger
 *
 * The host asks for an update by holding the serial line down with
 * OTA_TRIGGER_BYTE. The application acts on it only after several
 * consecutive polls have seen that character and nothing else.
 *
 * A single byte was the first version and it was not safe. This
 * reboots the board into a bootloader listen window, and the only
 * thing between a stray byte and that reboot was the byte's value: a
 * terminal left open on the port, one keystroke, a byte still in RDR
 * from before the jump. Everywhere else in this project the host link
 * is framed, sequenced and CRC'd precisely because a bare byte stream
 * cannot be trusted; the trigger had no business being the exception.
 *
 * The obvious repair -- a multi-byte magic word -- does not work on
 * this receive path. With no interrupt and no ring buffer, each pass
 * reads at most one byte and RDR holds exactly one; a magic word sent
 * in a single write arrives in forty microseconds and everything past
 * its first byte is lost to the overrun. Matching a sequence needs a
 * driver this application deliberately does not carry.
 *
 * So the confirmation is built along the axis that is available:
 * time. A host holding the line down for a couple of seconds passes;
 * a stray byte scores one poll and the next one erases it. Losing
 * bytes to overrun is harmless because the host keeps sending -- which
 * is exactly what makes this fit a buffer-less receive path, instead
 * of fighting it.
 * ---------------------------------------------------------------- */

static int ota_trigger_poll(void)
{
    static int streak = 0;

    uint8_t c;
    int seen  = 0;
    int clean = 1;

    /* Drain what is available this pass rather than reading a single
       byte: a poll that sees the trigger character followed by
       something else must not count towards the streak.

       The drain is bounded. This runs under the watchdog, and a host
       streaming without pause must not be able to hold the loop here.
       In practice the bound is never reached -- at 115200 baud a byte
       lands every 87 us and this loop takes a few microseconds -- but
       that is a statement about the CPU being faster than the line,
       and the loop's exit should not depend on it. */
    int budget = 16;

    while (budget-- > 0 && uart_getc(&c)) {
        seen = 1;
        if (c != (uint8_t)OTA_TRIGGER_BYTE) {
            clean = 0;
        }
    }

    if (seen && clean) {
        streak++;
    } else {
        streak = 0;
    }

    if (streak >= OTA_CONFIRM_POLLS) {
        streak = 0;
        return 1;
    }

    return 0;
}


/* ----------------------------------------------------------------
 * Determining which slot is executing
 * ---------------------------------------------------------------- */

static uint8_t current_slot(void)
{
    /* SCB_VTOR holds the vector table address, set by the bootloader
       just before jumping. It therefore points at the start of the
       slot currently executing.

       This introspection lets the application know where it runs
       without being told -- useful to confirm that the right binary
       reached the right slot. */
    return (SCB_VTOR >= SLOT_B_ADDR) ? SLOT_B : SLOT_A;
}


/* ----------------------------------------------------------------
 * Start-up confirmation
 * ---------------------------------------------------------------- */

static void confirm_startup(void)
{
    metadata_t meta;

    if (!metadata_read(&meta)) {
        uart_puts("  metadata unreadable, cannot confirm\r\n");
        return;
    }

    uint8_t self_slot = current_slot();

    if (meta.slot[self_slot].state == STATE_VALID && meta.boot_fail_count == 0U) {
        uart_puts("  already confirmed\r\n");
        return;
    }

    /* Only MY slot's state changes. The other one describes an image
       this build knows nothing about, and which must stay usable as a
       fallback. */
    meta.slot[self_slot].state = STATE_VALID;
    meta.boot_fail_count = 0;

    if (metadata_write(&meta) == META_OK) {
        uart_puts("  start-up confirmed: state VALID, counter cleared\r\n");
    } else {
        uart_puts("  metadata write failed\r\n");
    }
}


/* ----------------------------------------------------------------
 * Self-integrity check
 * ---------------------------------------------------------------- */

static void verify_image(const metadata_t *meta)
{
    uint8_t self_slot = current_slot();
    uint32_t base = SLOT_ADDR(self_slot);
    const slot_info_t *info = &meta->slot[self_slot];

    if (info->size == 0U || info->size > SLOT_SIZE) {
        uart_puts("  invalid size, check skipped\r\n");
        return;
    }

    uint32_t computed = crc32_compute((const uint8_t *)base, info->size);

    uart_puts("  computed CRC : ");
    uart_hex32(computed);
    uart_puts("\r\n  expected CRC : ");
    uart_hex32(info->crc32);
    uart_puts(computed == info->crc32 ? "   match\r\n"
                                     : "   MISMATCH\r\n");
}


/* ----------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------- */

int main(void)
{
    /* User LED on PA5 */
    RCC_AHB2ENR |= (1U << 0);
    GPIOA_MODER &= ~(3U << (LED_PIN * 2));
    GPIOA_MODER |=  (1U << (LED_PIN * 2));

    uart_init();
    crc32_init();

    uint8_t slot = current_slot();

    uart_puts("\r\n########################################\r\n");
    uart_puts("  APPLICATION -- slot ");
    uart_putc((char)('A' + slot));
    uart_puts("\r\n########################################\r\n");

    uart_puts("VTOR        : ");
    uart_hex32(SCB_VTOR);
    uart_puts("\r\n");

    metadata_t meta;
    if (metadata_read(&meta)) {
        uart_puts("State       : ");
        switch (meta.slot[current_slot()].state) {
        case STATE_EMPTY:       uart_puts("EMPTY");       break;
        case STATE_IN_PROGRESS: uart_puts("IN_PROGRESS"); break;
        case STATE_TESTING:     uart_puts("TESTING");     break;
        case STATE_VALID:       uart_puts("VALID");       break;
        default:                uart_dec(meta.slot[current_slot()].state); break;
        }
        uart_puts("\r\nVersion     : ");
        uart_hex32(meta.slot[current_slot()].version);
        uart_puts("\r\nBoot fails  : ");
        uart_dec(meta.boot_fail_count);
        uart_puts("\r\n\r\nImage verification:\r\n");
        verify_image(&meta);
    } else {
        uart_puts("No readable metadata\r\n");
    }

    uart_puts("\r\nApplication cycles before confirming: ");
    uart_dec(CYCLES_BEFORE_CONFIRMATION);
    uart_puts("\r\n\r\n");

    uint32_t cycle     = 0;
    int      confirmed = 0;
    uint32_t led_before;

    while (1) {
        /* Evidence accumulator for this pass.
         *
         * The previous version refreshed the watchdog on
         * `cycle != last_cycle_seen`, with `cycle` incremented on the
         * line above -- a condition that could never be false. It was
         * iwdg_feed() in a health check's clothing, and it defeated
         * the entire reason iwdg_feed_if() exists.
         *
         * Each unit of work the cycle owes now sets its own bit, and
         * the refresh happens only if all of them did. What makes this
         * falsifiable rather than decorative is that the bits come
         * from OBSERVED hardware state, not from having reached a
         * line: the blink bit is read back out of ODR, and the report
         * bit comes from the USART's TC flag, which only sets once the
         * last stop bit is physically on the wire.
         *
         * So an edit that returns early, a GPIO clock that gets gated
         * off, or a USART that stops shifting all withhold the refresh
         * and the watchdog resets the board -- which is exactly the
         * rollback path this application exists to exercise. */
        uint32_t work = 0;

        led_before = (GPIOA_ODR >> LED_PIN) & 1U;
        GPIOA_ODR ^= (1U << LED_PIN);
        if (((GPIOA_ODR >> LED_PIN) & 1U) != led_before) {
            work |= WORK_BLINK;
        }

        delay(300000);

        cycle++;

        uart_puts("cycle ");
        uart_dec(cycle);
        uart_puts("\r\n");

        /* Evidence that the report physically left the USART.
         *
         * TC cannot be sampled immediately: uart_putc() waits on TXE,
         * and writing TDR CLEARS TC. Right after the last character is
         * handed over, TC is always clear and only sets one character
         * time later, when the final stop bit is on the wire. Testing
         * it without waiting is a check that can never pass.
         *
         * So wait for it, but under a bound. The bound is what keeps
         * this a health check rather than a blocking call that always
         * succeeds eventually: a USART whose clock has been gated off
         * or whose shift register has stalled never sets TC, the
         * budget expires, the bit stays clear and the refresh is
         * withheld.
         *
         * The budget only has to be comfortably longer than one
         * character: 87 us at 115200 baud is roughly 350 core cycles
         * at 4 MHz, so this is two orders of magnitude of headroom.
         * Unlike a delay loop, its exact duration does not matter --
         * only that it is finite. */
        uint32_t tc_budget = 50000U;
        while (tc_budget-- > 0U) {
            if (USART2_ISR & (1U << 6)) {   /* TC */
                work |= WORK_REPORT;
                break;
            }
        }

        /* OTA trigger. Confirmed over several polls before it is acted
           on -- see the section above. The host side is
           tools/ota_flash.py --via-uart, which simply holds the line
           down for long enough. */
        if (ota_trigger_poll()) {
            uart_puts("\r\nOTA request confirmed -- resetting to bootloader\r\n");
            boot_request_set();
            software_reset();
        }
        /* Evidence that the protocol link is still configured.
         *
         * This bit used to be set unconditionally, immediately after
         * the trigger poll -- which made it exactly the tautology this
         * accumulator was written to replace. A bit that is always set
         * contributes nothing to WORK_ALL and only makes the check
         * look more thorough than it is.
         *
         * There is no honest evidence that "the trigger poll found
         * something", because finding nothing is the normal case. What
         * can be checked is that the peripheral it polls is still
         * alive: UE clear means USART1 has been disabled or its clock
         * gated behind the application's back, and the OTA path is
         * silently dead. That is worth a reset. */
        if (USART1_CR1 & (1U << 0)) {       /* UE */
            work |= WORK_LINK;
        }

        /* Confirmation only happens after several complete cycles.
           Confirming on the first line of main() would validate an
           application that crashes immediately afterwards. */
        if (!confirmed && cycle >= CYCLES_BEFORE_CONFIRMATION) {
            uart_puts("\r\nConfirmation:\r\n");
            confirm_startup();
            uart_puts("\r\n");
            confirmed = 1;
        }

        /* Refresh last, once the evidence is in. */
        iwdg_feed_if(work == WORK_ALL);
    }
}
