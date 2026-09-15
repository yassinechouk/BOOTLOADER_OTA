#include <stdint.h>
#include "iwdg.h"

/* ----------------------------------------------------------------
 * Registers -- RM0351 section 36.4
 * ---------------------------------------------------------------- */
#define IWDG_BASE           0x40003000UL

#define IWDG_KR             (*(volatile uint32_t *)(IWDG_BASE + 0x00))
#define IWDG_PR             (*(volatile uint32_t *)(IWDG_BASE + 0x04))
#define IWDG_RLR            (*(volatile uint32_t *)(IWDG_BASE + 0x08))
#define IWDG_SR             (*(volatile uint32_t *)(IWDG_BASE + 0x0C))
#define IWDG_WINR           (*(volatile uint32_t *)(IWDG_BASE + 0x10))

/* Key values -- RM0351 section 36.4.1 */
#define KEY_RELOAD          0xAAAAU     /* refresh the counter        */
#define KEY_UNLOCK          0x5555U     /* unlock PR, RLR and WINR    */
#define KEY_START           0xCCCCU     /* start the watchdog         */

/* IWDG_SR -- update-in-progress flags */
#define SR_PVU              (1U << 0)   /* prescaler update ongoing   */
#define SR_RVU              (1U << 1)   /* reload update ongoing      */
#define SR_WVU              (1U << 2)   /* window update ongoing      */

/* Window disabled: reset value of IWDG_WINR */
#define WINR_DISABLED       0x0FFFU

/* Reload register is 12 bits */
#define RLR_MAX             0x0FFFU

/* Nominal LSI frequency. The oscillator is specified at +/-5%
   (DS10198), which is accounted for in the timeout margin rather
   than here. */
#define LSI_FREQ_HZ         32000UL

/* ----------------------------------------------------------------
 * RCC -- reset cause flags, RM0351 section 6.4.29
 * ---------------------------------------------------------------- */
#define RCC_BASE            0x40021000UL
#define RCC_CSR             (*(volatile uint32_t *)(RCC_BASE + 0x94))

#define CSR_RMVF            (1U << 23)  /* remove reset flags         */
#define CSR_IWDGRSTF        (1U << 29)  /* independent watchdog reset */

/* ----------------------------------------------------------------
 * DBGMCU -- freeze peripherals while halted, RM0351 section 48.9
 * ---------------------------------------------------------------- */
#define DBGMCU_BASE         0xE0042000UL
#define DBGMCU_APB1FZR1     (*(volatile uint32_t *)(DBGMCU_BASE + 0x08))

#define DBG_IWDG_STOP       (1U << 12)


/* ----------------------------------------------------------------
 * Prescaler selection
 * ---------------------------------------------------------------- */

/* PR encoding: 0 -> /4, 1 -> /8, ... 6 -> /256 (7 also /256). */
static uint32_t prescaler_value(uint32_t pr)
{
    return 4UL << pr;
}


void iwdg_start(uint32_t timeout_ms)
{
    /* Find the smallest prescaler whose reload value fits in 12 bits.
       A smaller divider gives finer granularity, so we take the first
       one that works rather than the first that is large enough. */
    uint32_t pr = 0;
    uint32_t reload = 0;

    for (pr = 0; pr <= 6U; pr++) {
        uint32_t div = prescaler_value(pr);

        /* reload = timeout_ms * f_LSI / (1000 * div) - 1
           Ordering avoids overflow on 32 bits and keeps precision:
           timeout_ms is at most a few tens of thousands. */
        uint32_t ticks = (timeout_ms * (LSI_FREQ_HZ / 1000UL)) / div;

        if (ticks == 0U) {
            ticks = 1U;         /* shortest expressible timeout */
        }

        if (ticks <= (RLR_MAX + 1U)) {
            reload = ticks - 1U;
            break;
        }
    }

    if (pr > 6U) {
        /* Requested timeout exceeds what the peripheral can express
           (about 32.8 s at nominal LSI). Clamp to the maximum rather
           than let the value wrap silently. */
        pr = 6U;
        reload = RLR_MAX;
    }

    /* Sequence from RM0351 section 36.3.2, window option disabled. */

    IWDG_KR = KEY_START;        /* 1. start */
    IWDG_KR = KEY_UNLOCK;       /* 2. unlock PR / RLR / WINR */

    IWDG_PR  = pr;              /* 3. prescaler */
    IWDG_RLR = reload;          /* 4. reload value */

    /* 5. Wait for the values to propagate to the VDD domain. The
       update takes up to five LSI cycles; writing WINR or refreshing
       before it completes would be ignored. */
    while (IWDG_SR & (SR_PVU | SR_RVU | SR_WVU)) {
    }

    /* Window left at its reset value keeps the window option off:
       only a refresh that comes too *late* triggers a reset, never
       one that comes too early. */
    IWDG_WINR = WINR_DISABLED;

    IWDG_KR = KEY_RELOAD;       /* 6. load the counter */
}


void iwdg_feed(void)
{
    /* Writing KEY_RELOAD reloads the counter from RLR. It also
       re-locks PR and RLR, which is why unlocking is part of the
       start sequence and not done once at init. */
    IWDG_KR = KEY_RELOAD;
}


void iwdg_feed_if(int healthy)
{
    /* The distinction between this and iwdg_feed() is the whole
       point of the module.
     *
     * An unconditional refresh only proves that execution reaches a
     * particular line. A program stuck in a loop that happens to
     * contain that line keeps the watchdog quiet while doing nothing
     * useful -- the failure the watchdog was meant to catch.
     *
     * Refreshing only on a health condition turns a liveness check
     * into a correctness check. Under an RTOS this is typically a
     * supervisor task verifying that every other task has made
     * progress since the last cycle.
     */
    if (healthy) {
        IWDG_KR = KEY_RELOAD;
    }
}


int iwdg_caused_last_reset(void)
{
    return (RCC_CSR & CSR_IWDGRSTF) ? 1 : 0;
}


void iwdg_clear_reset_flags(void)
{
    /* RMVF is self-clearing but the flags are not: left set, every
       subsequent boot would report a watchdog reset. */
    RCC_CSR |= CSR_RMVF;
}


void iwdg_freeze_on_debug(void)
{
    /* RM0351 section 36.3.6: with this bit set the counter stops
       while the core is halted.
     *
     * Without it, a breakpoint resets the board a few seconds later
     * and step-by-step debugging becomes impossible -- a failure that
     * looks like a firmware bug and wastes hours.
     *
     * This is a debug aid. A shipped product should not enable it,
     * since it would also mask a genuine hang reached through the
     * debug interface.
     */
    DBGMCU_APB1FZR1 |= DBG_IWDG_STOP;
}
