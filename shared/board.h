#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>

/*
 * Board and clock configuration -- Nucleo-L476RG.
 *
 * Single source of truth for facts about the hardware that more than
 * one translation unit needs. Before this file existed the system
 * clock was stated independently in three places (the bootloader's
 * main.c, the UART driver, and the application's private UART block);
 * adding a PLL would have fixed two of them and left the third
 * generating a silently wrong baud rate.
 *
 * Clock
 * -----
 * Neither image configures the clock tree: both run on the MSI at its
 * reset default of 4 MHz. That is a deliberate simplification, not an
 * oversight -- the only timing requirements here are a UART baud rate
 * and a millisecond tick, and both are met at 4 MHz. Raising the clock
 * would mainly speed up the boot-time CRC over the active image, which
 * is the one operation that scales with image size.
 *
 * If a PLL is ever configured, SYSTEM_CLOCK_HZ is the only value that
 * has to change, and USART_BRR_OVER16() re-derives from it.
 */
#define SYSTEM_CLOCK_HZ     4000000UL   /* MSI, reset default */

/*
 * USART BRR value for oversampling by 16 (OVER8 = 0).
 *
 * RM0351 section 40.5.4: with OVER8 = 0, BRR = USARTDIV and
 * Tx/Rx baud = f_CK / USARTDIV.
 *
 * ROUNDED, not truncated. The difference is not cosmetic at this
 * clock: at 4 MHz and 115200 baud the exact divisor is 34.72, and
 *
 *     truncating -> 34 -> 117647 baud -> +2.12 % error
 *     rounding   -> 35 -> 114286 baud -> -0.79 % error
 *
 * A UART tolerates roughly +/-3 % end to end over ten bit periods, and
 * that budget has to cover the MSI's own drift over temperature as
 * well as the divisor. Spending two thirds of it on an arithmetic
 * choice that costs nothing to make correctly is not a trade, it is an
 * oversight. Rounding leaves the budget for the oscillator.
 */
#define USART_BRR_OVER16(fck, baud)     (((fck) + ((baud) / 2U)) / (baud))

/*
 * RM0351 section 40.5.4 requires USARTDIV >= 16 when oversampling by
 * 16 or 8: below that the sampling point cannot be placed correctly.
 * Exposed so a future clock or baud change can be checked rather than
 * silently producing a link that only misbehaves on the wire.
 */
#define USART_BRR_IS_VALID(fck, baud)   (USART_BRR_OVER16(fck, baud) >= 16U)

#endif /* BOARD_H */
