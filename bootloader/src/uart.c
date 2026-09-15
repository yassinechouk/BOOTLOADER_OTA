#include <stdint.h>
#include "uart.h"
#include "board.h"

/* ----------------------------------------------------------------
 * Addresses -- RM0351 section 2.2.2
 * ---------------------------------------------------------------- */
#define RCC_BASE            0x40021000UL
#define GPIOA_BASE          0x48000000UL
#define USART2_BASE         0x40004400UL
#define NVIC_ISER1          (*(volatile uint32_t *)0xE000E104UL)

#define RCC_AHB2ENR         (*(volatile uint32_t *)(RCC_BASE + 0x4C))
#define RCC_APB1ENR1        (*(volatile uint32_t *)(RCC_BASE + 0x58))

#define GPIOA_MODER         (*(volatile uint32_t *)(GPIOA_BASE + 0x00))
#define GPIOA_OSPEEDR       (*(volatile uint32_t *)(GPIOA_BASE + 0x08))
#define GPIOA_PUPDR         (*(volatile uint32_t *)(GPIOA_BASE + 0x0C))
#define GPIOA_AFRL          (*(volatile uint32_t *)(GPIOA_BASE + 0x20))

#define USART2_CR1          (*(volatile uint32_t *)(USART2_BASE + 0x00))
#define USART2_CR3          (*(volatile uint32_t *)(USART2_BASE + 0x08))
#define USART2_BRR          (*(volatile uint32_t *)(USART2_BASE + 0x0C))
#define USART2_ISR_REG      (*(volatile uint32_t *)(USART2_BASE + 0x1C))
#define USART2_ICR          (*(volatile uint32_t *)(USART2_BASE + 0x20))
#define USART2_RDR          (*(volatile uint32_t *)(USART2_BASE + 0x24))
#define USART2_TDR          (*(volatile uint32_t *)(USART2_BASE + 0x28))

/* USART_CR1 -- RM0351 section 40.8.1 */
#define CR1_UE              (1U << 0)
#define CR1_RE              (1U << 2)
#define CR1_TE              (1U << 3)
#define CR1_RXNEIE          (1U << 5)

/* USART_ISR -- RM0351 section 40.8.8 */
#define ISR_PE              (1U << 0)
#define ISR_FE              (1U << 1)
#define ISR_NE              (1U << 2)
#define ISR_ORE             (1U << 3)
#define ISR_RXNE            (1U << 5)
#define ISR_TC              (1U << 6)
#define ISR_TXE             (1U << 7)

/* USART_ICR -- flag clear register, write 1 to clear */
#define ICR_PECF            (1U << 0)
#define ICR_FECF            (1U << 1)
#define ICR_NECF            (1U << 2)
#define ICR_ORECF           (1U << 3)

/* System clock and BRR derivation live in shared/board.h. */

/* USART2 sits at position 38 in the vector table.
   NVIC_ISER1 covers interrupts 32 to 63, hence the shift. */
#define USART2_IRQ_NUMBER   38U


/* ----------------------------------------------------------------
 * Receive buffer
 *
 * head is written only by the ISR, tail only by the main context.
 * No variable is written by both, so no critical section is needed:
 * on Cortex-M, writing an aligned uint32_t is atomic.
 * ---------------------------------------------------------------- */
static volatile uint8_t  rx_buffer[UART_RX_BUFFER_SIZE];
static volatile uint32_t rx_head = 0;   /* written by the ISR        */
static volatile uint32_t rx_tail = 0;   /* written by main context   */

static volatile uint32_t cnt_overrun_sw = 0;
static volatile uint32_t cnt_overrun_hw = 0;
static volatile uint32_t cnt_framing    = 0;
static volatile uint32_t cnt_noise      = 0;

#define RX_MASK             (UART_RX_BUFFER_SIZE - 1U)


/* ----------------------------------------------------------------
 * Initialisation
 * ---------------------------------------------------------------- */

void uart_init(uint32_t baudrate)
{
    RCC_AHB2ENR  |= (1U << 0);      /* GPIOA clock */
    RCC_APB1ENR1 |= (1U << 17);     /* USART2 clock */

    /* PA2 and PA3 in alternate function mode (MODER = 10) */
    GPIOA_MODER &= ~((3U << (2 * 2)) | (3U << (3 * 2)));
    GPIOA_MODER |=  ((2U << (2 * 2)) | (2U << (3 * 2)));

    /* AF7 = USART2 (STM32L476 datasheet, AF mapping table) */
    GPIOA_AFRL &= ~((0xFU << (2 * 4)) | (0xFU << (3 * 4)));
    GPIOA_AFRL |=  ((7U   << (2 * 4)) | (7U   << (3 * 4)));

    /* Pull-up on RX: holds the line high when the transmitter is
       disconnected, preventing a flood of spurious bytes. */
    GPIOA_PUPDR &= ~(3U << (3 * 2));
    GPIOA_PUPDR |=  (1U << (3 * 2));

    /* High speed: not critical at 115200 baud, but reduces rise
       time and therefore noise sensitivity. */
    GPIOA_OSPEEDR |= (3U << (2 * 2)) | (3U << (3 * 2));

    /* USART must be disabled to reconfigure BRR. */
    USART2_CR1 = 0;
    USART2_CR3 = 0;

    /* Oversampling by 16 (OVER8 = 0): BRR = USARTDIV = f_CK / baud,
       rounded rather than truncated -- see USART_BRR_OVER16() in
       shared/board.h for why the difference matters at this clock. */
    USART2_BRR = USART_BRR_OVER16(SYSTEM_CLOCK_HZ, baudrate);

    rx_head = 0;
    rx_tail = 0;

    /* Clear any error flags inherited from a previous session. */
    USART2_ICR = ICR_PECF | ICR_FECF | ICR_NECF | ICR_ORECF;

    /* Enable the USART2 interrupt in the NVIC before arming RXNEIE,
       to avoid missing a byte that arrives immediately. */
    NVIC_ISER1 = (1U << (USART2_IRQ_NUMBER - 32U));

    USART2_CR1 = CR1_TE | CR1_RE | CR1_RXNEIE | CR1_UE;
}


/* ----------------------------------------------------------------
 * Interrupt handler
 *
 * The name matches the vector table entry in startup.s. The weak
 * symbol defined there is automatically replaced by this strong
 * definition.
 * ---------------------------------------------------------------- */

void USART2_IRQHandler(void)
{
    uint32_t status = USART2_ISR_REG;

    /* Errors are handled first.
     *
     * ORE is the most severe: until it is explicitly cleared,
     * the USART STOPS receiving. A driver that ignores it goes deaf
     * after the first overrun, with no visible symptom on the
     * firmware side -- the PC simply sees its frames go unanswered. */
    if (status & (ISR_ORE | ISR_FE | ISR_NE | ISR_PE)) {

        if (status & ISR_ORE) {
            cnt_overrun_hw++;
            USART2_ICR = ICR_ORECF;
        }
        if (status & ISR_FE) {
            cnt_framing++;          /* missing stop bit: wrong baud rate */
            USART2_ICR = ICR_FECF;
        }
        if (status & ISR_NE) {
            cnt_noise++;            /* inconsistent sampling */
            USART2_ICR = ICR_NECF;
        }
        if (status & ISR_PE) {
            USART2_ICR = ICR_PECF;
        }
    }

    if (status & ISR_RXNE) {
        /* Reading RDR clears RXNE. It must happen even when the
           buffer is full, otherwise the interrupt would re-fire
           indefinitely. */
        uint8_t byte = (uint8_t)(USART2_RDR & 0xFFU);

        uint32_t next = (rx_head + 1U) & RX_MASK;

        if (next == rx_tail) {
            /* Buffer full: drop the new byte rather than overwriting
               the oldest. The current frame will be invalidated by
               its CRC and retransmitted; overwriting would corrupt a
               frame that is already complete. */
            cnt_overrun_sw++;
        } else {
            rx_buffer[rx_head] = byte;
            rx_head = next;
        }
    }
}


/* ----------------------------------------------------------------
 * Transmission
 * ---------------------------------------------------------------- */

void uart_putc(char c)
{
    while (!(USART2_ISR_REG & ISR_TXE)) {
        /* wait for the transmit register to be free */
    }
    USART2_TDR = (uint32_t)(uint8_t)c;
}


void uart_puts(const char *s)
{
    if (s == 0) {
        return;
    }
    while (*s) {
        uart_putc(*s++);
    }
}


void uart_write(const uint8_t *data, uint32_t len)
{
    if (data == 0) {
        return;
    }
    while (len--) {
        uart_putc((char)*data++);
    }
}


void uart_flush(void)
{
    /* TXE indicates the register is free, not that the byte has
       left. TC signals the actual end of transmission. */
    while (!(USART2_ISR_REG & ISR_TC)) {
    }
}


/* ----------------------------------------------------------------
 * Formatting
 * ---------------------------------------------------------------- */

static const char HEX_DIGITS[] = "0123456789ABCDEF";

void uart_hex32(uint32_t v)
{
    uart_puts("0x");
    for (int i = 28; i >= 0; i -= 4) {
        uart_putc(HEX_DIGITS[(v >> i) & 0xFU]);
    }
}


void uart_hex8(uint8_t v)
{
    uart_putc(HEX_DIGITS[(v >> 4) & 0xFU]);
    uart_putc(HEX_DIGITS[v & 0xFU]);
}


void uart_dec(uint32_t v)
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


/* ----------------------------------------------------------------
 * Reception
 * ---------------------------------------------------------------- */

uint32_t uart_available(void)
{
    /* Single read of each index: head may change under us, but the
       value read remains consistent and at worst underestimates the
       number of available bytes. */
    uint32_t head = rx_head;
    uint32_t tail = rx_tail;

    return (head - tail) & RX_MASK;
}


int uart_getc(uint8_t *out)
{
    if (out == 0) {
        return 0;
    }

    uint32_t tail = rx_tail;

    if (rx_head == tail) {
        return 0;                   /* buffer empty */
    }

    *out = rx_buffer[tail];
    rx_tail = (tail + 1U) & RX_MASK;

    return 1;
}


uint32_t uart_read(uint8_t *dest, uint32_t max)
{
    if (dest == 0) {
        return 0;
    }

    uint32_t n = 0;
    while (n < max && uart_getc(&dest[n])) {
        n++;
    }
    return n;
}


void uart_rx_flush(void)
{
    rx_tail = rx_head;
}


/* ----------------------------------------------------------------
 * Diagnostics
 * ---------------------------------------------------------------- */

uint32_t uart_overrun_count(void)       { return cnt_overrun_sw; }
uint32_t uart_hw_overrun_count(void)    { return cnt_overrun_hw; }
uint32_t uart_framing_error_count(void) { return cnt_framing;    }
uint32_t uart_noise_error_count(void)   { return cnt_noise;      }

void uart_reset_counters(void)
{
    cnt_overrun_sw = 0;
    cnt_overrun_hw = 0;
    cnt_framing    = 0;
    cnt_noise      = 0;
}
