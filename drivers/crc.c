#include <stdint.h>
#include "crc.h"

/* ----------------------------------------------------------------
 * Register addresses -- RM0351 section 2.2.2
 * ---------------------------------------------------------------- */
#define RCC_BASE            0x40021000UL
#define CRC_BASE            0x40023000UL

/* RM0351 section 6.4.16 : AHB1 peripheral clock enable register */
#define RCC_AHB1ENR         (*(volatile uint32_t *)(RCC_BASE + 0x48))
#define RCC_AHB1ENR_CRCEN   (1U << 12)

/* RM0351 section 15.4 : CRC peripheral registers.
 *
 * CRC_DR is accessible as a word, right-aligned half-word and
 * right-aligned byte. Both useful widths are exposed here. */
#define CRC_DR_WORD         (*(volatile uint32_t *)(CRC_BASE + 0x00))
#define CRC_DR_BYTE         (*(volatile uint8_t  *)(CRC_BASE + 0x00))
#define CRC_CR              (*(volatile uint32_t *)(CRC_BASE + 0x08))
#define CRC_INIT_REG        (*(volatile uint32_t *)(CRC_BASE + 0x10))
#define CRC_POL_REG         (*(volatile uint32_t *)(CRC_BASE + 0x14))

/* CRC_CR -- RM0351 section 15.4.3 */
#define CRC_CR_RESET        (1U << 0)   /* rs: cleared automatically in hardware */
#define CRC_CR_REV_IN_BYTE  (1U << 5)   /* REV_IN[1:0] = 01 */

#define CRC_POLYNOMIAL      0x04C11DB7UL
#define CRC_INIT_VALUE      0xFFFFFFFFUL


void crc32_init(void)
{
    RCC_AHB1ENR |= RCC_AHB1ENR_CRCEN;

    CRC_POL_REG  = CRC_POLYNOMIAL;
    CRC_INIT_REG = CRC_INIT_VALUE;

    /* REV_IN = 01, REV_OUT = 0, POLYSIZE = 00 (32 bits) */
    CRC_CR = CRC_CR_REV_IN_BYTE;
}


void crc32_reset(void)
{
    /* Direct write rather than |=.
     *
     * RESET is an "rs" bit: set by software and cleared automatically
     * by hardware. A |= would perform a read-modify-write whose result
     * depends on the moment of the read. A direct write is deterministic.
     *
     * REV_IN must be rewritten explicitly; otherwise it would be lost
     * and all subsequent CRCs would be wrong. */
    CRC_CR = CRC_CR_REV_IN_BYTE | CRC_CR_RESET;
}


void crc32_update(const uint8_t *data, uint32_t len)
{
    /* Byte-by-byte feeding.
     *
     * The peripheral consumes one AHB cycle per byte regardless of
     * access width: 1 cycle for a byte, 4 for a word. Throughput is
     * therefore identical, and word-wide writes would bring no gain --
     * they would only save loop iterations, negligible compared to
     * the peripheral's own time.
     *
     * Word writes would also require handling source pointer alignment
     * (a 32-bit unaligned access is undefined on Cortex-M) and the
     * residual bytes of a length that is not a multiple of 4. Two
     * sources of bugs in the module on which all protocol validation
     * depends. */
    while (len--) {
        CRC_DR_BYTE = *data++;
    }
}


uint32_t crc32_get(void)
{
    return CRC_DR_WORD;
}


uint32_t crc32_compute(const uint8_t *data, uint32_t len)
{
    crc32_reset();
    crc32_update(data, len);
    return crc32_get();
}
