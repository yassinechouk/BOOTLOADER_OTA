#include <stdint.h>
#include "uart.h"

/* ----------------------------------------------------------------
 * Addresses — RM0351 section 2.2.2
 * ---------------------------------------------------------------- */
#define RCC_BASE            0x40021000UL
#define GPIOA_BASE          0x48000000UL
#define USART1_BASE         0x40013800UL    /* APB2 */
#define USART2_BASE         0x40004400UL    /* APB1 */

#define RCC_AHB2ENR         (*(volatile uint32_t *)(RCC_BASE + 0x4C))
#define RCC_APB1ENR1        (*(volatile uint32_t *)(RCC_BASE + 0x58))
#define RCC_APB2ENR         (*(volatile uint32_t *)(RCC_BASE + 0x60))

#define RCC_AHB2ENR_GPIOAEN  (1U << 0)
#define RCC_APB1ENR1_USART2EN (1U << 17)
#define RCC_APB2ENR_USART1EN  (1U << 14)

#define GPIOA_MODER         (*(volatile uint32_t *)(GPIOA_BASE + 0x00))
#define GPIOA_OSPEEDR       (*(volatile uint32_t *)(GPIOA_BASE + 0x08))
#define GPIOA_PUPDR         (*(volatile uint32_t *)(GPIOA_BASE + 0x0C))
#define GPIOA_AFRL          (*(volatile uint32_t *)(GPIOA_BASE + 0x20))
#define GPIOA_AFRH          (*(volatile uint32_t *)(GPIOA_BASE + 0x24))

/* Register offsets in 32-bit words, for indexing through base[]. */
#define R_CR1               (0x00U / 4U)
#define R_CR3               (0x08U / 4U)
#define R_BRR               (0x0CU / 4U)
#define R_ISR               (0x1CU / 4U)
#define R_ICR               (0x20U / 4U)
#define R_RDR               (0x24U / 4U)
#define R_TDR               (0x28U / 4U)

#define CR1_UE              (1U << 0)
#define CR1_RE              (1U << 2)
#define CR1_TE              (1U << 3)
#define CR1_RXNEIE          (1U << 5)

#define ISR_PE              (1U << 0)
#define ISR_FE              (1U << 1)
#define ISR_NE              (1U << 2)
#define ISR_ORE             (1U << 3)
#define ISR_RXNE            (1U << 5)
#define ISR_TC              (1U << 6)
#define ISR_TXE             (1U << 7)

#define ICR_PECF            (1U << 0)
#define ICR_FECF            (1U << 1)
#define ICR_NECF            (1U << 2)
#define ICR_ORECF           (1U << 3)

#define NVIC_ISER1          (*(volatile uint32_t *)0xE000E104UL)
#define USART1_IRQ_NUMBER   37U
#define USART2_IRQ_NUMBER   38U

#define SYSTEM_CLOCK_HZ     4000000UL       /* MSI at reset */


/* ----------------------------------------------------------------
 * Instances
 *
 * uart_debug carries no buffer: rx_buf NULL marks it transmit-only,
 * and every receive entry point checks that before touching anything.
 * ---------------------------------------------------------------- */

static volatile uint8_t proto_rx_buffer[UART_RX_BUFFER_SIZE];

uart_t uart_debug = {
    .base    = (volatile uint32_t *)USART2_BASE,
    .rx_buf  = 0,
    .rx_mask = 0,
};

uart_t uart_proto = {
    .base    = (volatile uint32_t *)USART1_BASE,
    .rx_buf  = proto_rx_buffer,
    .rx_mask = UART_RX_BUFFER_SIZE - 1U,
};


/* ----------------------------------------------------------------
 * Shared configuration
 * ---------------------------------------------------------------- */

static void uart_configure(uart_t *u, uint32_t baudrate, int with_rx)
{
    volatile uint32_t *r = u->base;

    /* The peripheral must be disabled to write BRR. */
    r[R_CR1] = 0;
    r[R_CR3] = 0;

    /* Oversampling by 16, the reset default: BRR = f_ck / baudrate.
       At 4 MHz and 115200 this gives 34, so 117647 baud in practice —
       2.1 % off, well inside what a UART tolerates. */
    r[R_BRR] = baudrate ? (SYSTEM_CLOCK_HZ / baudrate) : 34U;

    u->rx_head = 0;
    u->rx_tail = 0;

    /* Clear any error flag inherited from a previous run. */
    r[R_ICR] = ICR_PECF | ICR_FECF | ICR_NECF | ICR_ORECF;

    if (with_rx && u->rx_buf) {
        r[R_CR1] = CR1_TE | CR1_RE | CR1_RXNEIE | CR1_UE;
    } else {
        /* No RE: with the receiver off there is no overrun to service
           and no interrupt to arm. */
        r[R_CR1] = CR1_TE | CR1_UE;
    }
}


void uart_debug_init(uint32_t baudrate)
{
    RCC_AHB2ENR  |= RCC_AHB2ENR_GPIOAEN;
    RCC_APB1ENR1 |= RCC_APB1ENR1_USART2EN;      /* APB1, not APB2 */

    /* PA2 = TX, PA3 = RX, alternate function 7.
       PA3 is configured even though the receiver stays off: leaving it
       as a floating analog input next to an active transmitter is how
       crosstalk gets in. */
    GPIOA_MODER &= ~((3U << (2 * 2)) | (3U << (3 * 2)));
    GPIOA_MODER |=  ((2U << (2 * 2)) | (2U << (3 * 2)));

    GPIOA_AFRL  &= ~((0xFU << (2 * 4)) | (0xFU << (3 * 4)));
    GPIOA_AFRL  |=  ((7U   << (2 * 4)) | (7U   << (3 * 4)));

    GPIOA_OSPEEDR |= (3U << (2 * 2)) | (3U << (3 * 2));

    uart_configure(&uart_debug, baudrate, 0);
}


void uart_proto_init(uint32_t baudrate)
{
    RCC_AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
    RCC_APB2ENR |= RCC_APB2ENR_USART1EN;        /* APB2, not APB1 */

    /* PA9 = TX, PA10 = RX, alternate function 7.
       These pins are above 7, so the alternate function goes in AFRH
       with a shift of (pin - 8) * 4. Using AFRL here would silently
       configure PA1 and PA2 instead — and PA2 belongs to the debug
       link, so the symptom would appear on the wrong channel. */
    GPIOA_MODER &= ~((3U << (9 * 2)) | (3U << (10 * 2)));
    GPIOA_MODER |=  ((2U << (9 * 2)) | (2U << (10 * 2)));

    GPIOA_AFRH  &= ~((0xFU << ((9 - 8) * 4)) | (0xFU << ((10 - 8) * 4)));
    GPIOA_AFRH  |=  ((7U   << ((9 - 8) * 4)) | (7U   << ((10 - 8) * 4)));

    /* Pull-up on RX: holds the line idle when the ESP32 is unpowered
       or unplugged. A floating receive pin generates a stream of
       framing errors that look like real traffic. */
    GPIOA_PUPDR &= ~(3U << (10 * 2));
    GPIOA_PUPDR |=  (1U << (10 * 2));

    GPIOA_OSPEEDR |= (3U << (9 * 2)) | (3U << (10 * 2));

    /* NVIC armed before RXNEIE, so a byte arriving immediately is not
       missed between the two writes. */
    NVIC_ISER1 = (1U << (USART1_IRQ_NUMBER - 32U));

    uart_configure(&uart_proto, baudrate, 1);
}


/* ----------------------------------------------------------------
 * Interrupt handling
 * ---------------------------------------------------------------- */

static void uart_isr(uart_t *u)
{
    volatile uint32_t *r = u->base;
    uint32_t status = r[R_ISR];

    /* Errors first.
     *
     * ORE is the one that matters: until it is cleared explicitly the
     * USART STOPS RECEIVING. A driver that ignores it goes deaf after
     * the first overrun with no symptom beyond frames going
     * unanswered — which looks exactly like a wiring fault. */
    if (status & (ISR_ORE | ISR_FE | ISR_NE | ISR_PE)) {

        if (status & ISR_ORE) {
            u->cnt_overrun_hw++;
            r[R_ICR] = ICR_ORECF;
        }
        if (status & ISR_FE) {
            u->cnt_framing++;           /* missing stop bit: wrong baud */
            r[R_ICR] = ICR_FECF;
        }
        if (status & ISR_NE) {
            u->cnt_noise++;
            r[R_ICR] = ICR_NECF;
        }
        if (status & ISR_PE) {
            r[R_ICR] = ICR_PECF;
        }
    }

    if (status & ISR_RXNE) {
        /* Reading RDR clears RXNE. It has to happen even when the
           buffer is full, or the interrupt would re-fire forever. */
        uint8_t byte = (uint8_t)(r[R_RDR] & 0xFFU);

        if (!u->rx_buf) {
            return;                     /* transmit-only instance */
        }

        uint32_t next = (u->rx_head + 1U) & u->rx_mask;

        if (next == u->rx_tail) {
            /* Buffer full: drop the incoming byte rather than
               overwrite the oldest. In a framed, CRC-checked protocol
               dropping the new byte corrupts the frame in flight —
               the CRC catches it and the host retransmits. Overwriting
               would corrupt a frame already complete and possibly
               being processed. */
            u->cnt_overrun_sw++;
        } else {
            u->rx_buf[u->rx_head] = byte;
            u->rx_head = next;
        }
    }
}


/* The NVIC passes no argument, so each vector gets a wrapper that
   supplies the instance. The body above is written once. */
void USART1_IRQHandler(void) { uart_isr(&uart_proto); }
void USART2_IRQHandler(void) { uart_isr(&uart_debug); }


/* ----------------------------------------------------------------
 * Transmission
 * ---------------------------------------------------------------- */

void uart_putc(uart_t *u, char c)
{
    volatile uint32_t *r = u->base;

    while (!(r[R_ISR] & ISR_TXE)) {
    }
    r[R_TDR] = (uint32_t)(uint8_t)c;
}


void uart_puts(uart_t *u, const char *s)
{
    if (!s) {
        return;
    }
    while (*s) {
        uart_putc(u, *s++);
    }
}


void uart_write(uart_t *u, const uint8_t *data, uint32_t len)
{
    if (!data) {
        return;
    }
    while (len--) {
        uart_putc(u, (char)*data++);
    }
}


void uart_flush(uart_t *u)
{
    volatile uint32_t *r = u->base;

    /* TXE says the register is free, TC says the line is idle. Only
       the second guarantees the last character left intact. */
    while (!(r[R_ISR] & ISR_TC)) {
    }
}


/* ----------------------------------------------------------------
 * Formatting
 * ---------------------------------------------------------------- */

static const char HEX_DIGITS[] = "0123456789ABCDEF";

void uart_hex32(uart_t *u, uint32_t v)
{
    uart_puts(u, "0x");
    for (int i = 28; i >= 0; i -= 4) {
        uart_putc(u, HEX_DIGITS[(v >> i) & 0xFU]);
    }
}


void uart_hex8(uart_t *u, uint8_t v)
{
    uart_putc(u, HEX_DIGITS[(v >> 4) & 0xFU]);
    uart_putc(u, HEX_DIGITS[v & 0xFU]);
}


void uart_dec(uart_t *u, uint32_t v)
{
    char tmp[10];
    int  n = 0;

    if (v == 0U) {
        uart_putc(u, '0');
        return;
    }
    while (v > 0U) {
        tmp[n++] = (char)('0' + (v % 10U));
        v /= 10U;
    }
    while (n-- > 0) {
        uart_putc(u, tmp[n]);
    }
}


/* ----------------------------------------------------------------
 * Reception
 * ---------------------------------------------------------------- */

uint32_t uart_available(uart_t *u)
{
    if (!u->rx_buf) {
        return 0;
    }

    /* Each index is read once. head may move underneath, but the
       value obtained stays coherent and at worst under-reports. */
    uint32_t head = u->rx_head;
    uint32_t tail = u->rx_tail;

    return (head - tail) & u->rx_mask;
}


int uart_getc(uart_t *u, uint8_t *out)
{
    if (!u->rx_buf || !out) {
        return 0;
    }

    uint32_t tail = u->rx_tail;

    if (u->rx_head == tail) {
        return 0;
    }

    *out = u->rx_buf[tail];
    u->rx_tail = (tail + 1U) & u->rx_mask;

    return 1;
}


uint32_t uart_read(uart_t *u, uint8_t *dest, uint32_t max)
{
    if (!dest) {
        return 0;
    }

    uint32_t n = 0;
    while (n < max && uart_getc(u, &dest[n])) {
        n++;
    }
    return n;
}


void uart_rx_flush(uart_t *u)
{
    if (u->rx_buf) {
        u->rx_tail = u->rx_head;
    }
}


/* ----------------------------------------------------------------
 * Diagnostics
 * ---------------------------------------------------------------- */

uint32_t uart_overrun_count(uart_t *u)       { return u->cnt_overrun_sw; }
uint32_t uart_hw_overrun_count(uart_t *u)    { return u->cnt_overrun_hw; }
uint32_t uart_framing_error_count(uart_t *u) { return u->cnt_framing;    }
uint32_t uart_noise_error_count(uart_t *u)   { return u->cnt_noise;      }

void uart_reset_counters(uart_t *u)
{
    u->cnt_overrun_sw = 0;
    u->cnt_overrun_hw = 0;
    u->cnt_framing    = 0;
    u->cnt_noise      = 0;
}
