#ifndef UART_H
#define UART_H

#include <stdint.h>

/*
 * USART driver — one implementation, several instances.
 *
 * The board now carries two independent serial links:
 *
 *   uart_debug  USART2 on PA2/PA3, wired to the ST-LINK by solder
 *               bridges SB13/SB14. Diagnostic output only.
 *
 *   uart_proto  USART1 on PA9/PA10, brought out on the ST morpho
 *               connector. Carries the update protocol, now to an
 *               ESP32 acting as a WiFi gateway.
 *
 * Why two rather than one
 * -----------------------
 * Moving the protocol to USART1 and dropping USART2 would have been
 * simpler, and it would have cost the only direct window into the
 * bootloader. Debugging the gateway would then mean reading the
 * bootloader's output through the gateway being debugged.
 *
 * Keeping both separates the channels: the protocol runs over the
 * link under test, while diagnostics keep flowing to a console that
 * does not depend on it.
 *
 * Why an instance struct rather than two modules
 * ----------------------------------------------
 * A second copy of uart.c with renamed symbols would have avoided
 * touching any caller, at the cost of two files sharing ninety
 * percent of their content — exactly the duplication the drivers/
 * refactor removed elsewhere. A ring buffer fix would have to be
 * applied twice, and the day one copy is forgotten the two links
 * behave differently for reasons nobody remembers.
 *
 * The state that differs between instances is small: a base address,
 * a buffer, four counters. Putting it in a struct writes the logic
 * once and makes a third port a declaration rather than a file.
 *
 * Asymmetric instances
 * --------------------
 * uart_debug is transmit-only. It has no receive buffer, no receive
 * interrupt, and RE is never set.
 *
 * Giving it a 512-byte buffer and an ISR "for symmetry" would spend
 * RAM and interrupt latency on a capability nothing uses, and would
 * add a source of spurious interrupts on a floating pin. Instances
 * are allowed to differ; the struct carries a NULL buffer and the
 * code treats that as "transmit only".
 *
 * Interrupt dispatch
 * ------------------
 * The NVIC calls USART1_IRQHandler and USART2_IRQHandler, neither of
 * which takes an argument. An ISR cannot discover which instance it
 * serves, so the handler body is written once and two thin wrappers
 * supply the context the hardware does not.
 */

/* Must be a power of two: the modulo then reduces to a bit mask, one
   instruction instead of a division, in code that runs per byte. */
#define UART_RX_BUFFER_SIZE     512U


typedef struct {
    volatile uint32_t *base;        /* peripheral base address        */

    /* NULL on a transmit-only instance. */
    volatile uint8_t  *rx_buf;
    uint32_t           rx_mask;     /* size - 1, size being a power of two */

    /* head is written only by the ISR, tail only by the main context.
       No variable is written by both, which is what removes the need
       for a critical section: on Cortex-M an aligned 32-bit store is
       atomic. One slot is sacrificed so that head == tail can mean
       empty unambiguously — the alternative, an element count, would
       be written by both sides. */
    volatile uint32_t  rx_head;
    volatile uint32_t  rx_tail;

    volatile uint32_t  cnt_overrun_sw;
    volatile uint32_t  cnt_overrun_hw;
    volatile uint32_t  cnt_framing;
    volatile uint32_t  cnt_noise;
} uart_t;


extern uart_t uart_debug;       /* USART2 -> ST-LINK, TX only        */
extern uart_t uart_proto;       /* USART1 -> ESP32, full duplex      */


/* --- Initialisation -------------------------------------------- */

/* Each sets up its own clock domain and pins, then shares the rest.
   USART1 lives on APB2, USART2 on APB1 — enabling the wrong one
   leaves the peripheral clock-gated and its register writes are
   discarded with no error at all. */
void uart_debug_init(uint32_t baudrate);
void uart_proto_init(uint32_t baudrate);

/* --- Transmission ---------------------------------------------- */

void uart_putc(uart_t *u, char c);
void uart_puts(uart_t *u, const char *s);
void uart_write(uart_t *u, const uint8_t *data, uint32_t len);

/* Waits for the transmission to actually finish (TC), not merely for
   the register to free up (TXE). Required before cutting the clock or
   jumping away, or the last character is truncated mid-frame. */
void uart_flush(uart_t *u);

/* --- Formatting ------------------------------------------------- */

void uart_hex32(uart_t *u, uint32_t v);
void uart_hex8(uart_t *u, uint8_t v);
void uart_dec(uart_t *u, uint32_t v);

/* --- Reception -------------------------------------------------- */
/* All return 0 on a transmit-only instance. */

uint32_t uart_available(uart_t *u);
int      uart_getc(uart_t *u, uint8_t *out);
uint32_t uart_read(uart_t *u, uint8_t *dest, uint32_t max);
void     uart_rx_flush(uart_t *u);

/* --- Diagnostics ------------------------------------------------ */

uint32_t uart_overrun_count(uart_t *u);       /* software buffer full */
uint32_t uart_hw_overrun_count(uart_t *u);    /* USART ORE flag       */
uint32_t uart_framing_error_count(uart_t *u); /* FE flag              */
uint32_t uart_noise_error_count(uart_t *u);   /* NE flag              */
void     uart_reset_counters(uart_t *u);

#endif /* UART_H */
