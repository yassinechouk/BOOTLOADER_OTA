#ifndef PROTOCOL_MGR_H
#define PROTOCOL_MGR_H

#include <stdint.h>
#include "protocol.h"
#include "metadata.h"

/*
 * Update protocol state machine.
 *
 * This module assembles incoming bytes into frames, validates each
 * one, executes the corresponding command and emits the response.
 * It does not know the transport layer: it consumes whatever
 * uart_getc() provides and transmits via uart_write(). Porting to
 * CAN would only touch those two points.
 *
 * This file is the specification. tests/host compiles it for the PC
 * against models of the flash, CRC and UART drivers, and drives it
 * through the edge cases; a Python reimplementation used to play that
 * role and had silently drifted from it.
 *
 * Lazy erase
 * ----------
 * Erasing the entire slot at the start of a transfer would be the
 * obvious approach. On silicon, 480 KB represents 240 pages at twenty
 * milliseconds each, nearly five seconds of unavailability --
 * for a firmware that may occupy only 20 KB.
 *
 * This module therefore erases page by page, just before writing.
 * The 256-byte block size divides the 2048-byte page exactly, so
 * every page boundary coincides with a block boundary: the test
 * reduces to offset % FLASH_PAGE_SIZE == 0.
 *
 * Injected time base
 * ------------------
 * protocol_poll() receives the current time as a parameter rather
 * than depending on a timer. The module remains testable on a PC,
 * and the choice of time source -- SysTick, TIM, other -- belongs
 * to the caller.
 */

typedef enum {
    PROTO_IDLE = 0,       /* no transfer in progress               */
    PROTO_RECEIVING,      /* transfer started, blocks expected     */
    PROTO_COMPLETE        /* valid firmware, reboot pending        */
} protocol_state_t;

/* Initialises the module. Must be called after uart_init() and crc32_init(). */
void protocol_init(void);

/*
 * Consumes available bytes and processes complete frames.
 * Must be called in a loop. now_ms is a monotonic time base in
 * milliseconds; its precision only matters for timeouts.
 */
void protocol_poll(uint32_t now_ms);

/* Current state of the state machine. */
protocol_state_t protocol_get_state(void);

/* True when a firmware has just been validated and the bootloader
   must reboot to execute it. */
int protocol_update_complete(void);

/* --- Diagnostics --- */
uint32_t protocol_frames_received(void);
uint32_t protocol_frames_rejected(void);
uint32_t protocol_bytes_written(void);

#endif /* PROTOCOL_MGR_H */
