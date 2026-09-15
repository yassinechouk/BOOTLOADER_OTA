#ifndef FAULT_H
#define FAULT_H

#include <stdint.h>

/*
 * Hard fault reporting.
 *
 * Without it every fault vector in startup.s resolves to
 * Default_Handler, an infinite loop. The watchdog then resets the
 * board three seconds later and the only evidence that anything went
 * wrong is a reboot. In a system whose recovery mechanism counts
 * failed boots, that is the difference between "the image is bad" and
 * "the image faulted at this PC for this reason" -- and only the
 * second one can be acted on.
 *
 * The handler prints the exception stack frame the processor pushed
 * automatically (R0-R3, R12, LR, PC, xPSR) together with the fault
 * status registers, then stops refreshing the watchdog and lets it
 * reset the board.
 *
 * Why it does not reset immediately
 * ---------------------------------
 * Letting the IWDG do it keeps one recovery path instead of two, and
 * the three-second delay gives the message time to leave the UART. In
 * the application the reset also advances boot_fail_count, so a
 * firmware that faults deterministically is rolled back by the
 * mechanism that already exists rather than by a special case here.
 *
 * Output
 * ------
 * The handler cannot depend on the interrupt-driven UART: the fault
 * may well have happened inside it. Each image supplies a blocking,
 * register-level character writer through fault_putc(). Nothing else
 * is called -- no formatting library, no buffering, no allocation.
 */

/* Provided by each image. Must be blocking, must not use interrupts,
   and must be safe to call from an exception context. */
void fault_putc(char c);

#endif /* FAULT_H */
