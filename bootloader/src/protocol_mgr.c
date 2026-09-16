#include <stdint.h>
#include "protocol_mgr.h"
#include "protocol.h"
#include "metadata.h"
#include "metadata_mgr.h"
#include "flash.h"
#include "crc.h"
#include "uart.h"

/* ----------------------------------------------------------------
 * Frame assembly
 *
 * Bytes arrive one at a time with no guaranteed framing. A state
 * machine accumulates them until a complete frame is available.
 * ---------------------------------------------------------------- */
typedef enum {
    RX_MAGIC0 = 0,
    RX_MAGIC1,
    RX_HEADER,      /* CMD, LENGTH, SEQ: 5 bytes */
    RX_DATA,
    RX_CRC          /* 4 bytes */
} rx_state_t;

static rx_state_t rx_state;
static uint8_t    rx_frame[FRAME_HEADER_SIZE + MAX_PAYLOAD_SIZE + FRAME_CRC_SIZE];
static uint32_t   rx_index;      /* position in rx_frame          */
static uint32_t   rx_expected;   /* bytes remaining for this state */

/* ----------------------------------------------------------------
 * Transfer context
 * ---------------------------------------------------------------- */
static protocol_state_t state;

static uint16_t expected_seq;
static uint16_t last_seq;
static uint8_t  target_slot;
static uint32_t fw_size;
static uint32_t fw_crc32;
static uint32_t fw_version;
static uint32_t bytes_written;

static uint32_t last_activity_ms;
static uint32_t timeout_ms;

static uint32_t cnt_frames_ok;
static uint32_t cnt_frames_ko;


/* ----------------------------------------------------------------
 * Frame transmission
 * ---------------------------------------------------------------- */

static void send_frame(uint8_t cmd, uint16_t seq,
                       const uint8_t *data, uint16_t len)
{
    uint8_t header[FRAME_HEADER_SIZE];

    header[0] = FRAME_MAGIC_0;
    header[1] = FRAME_MAGIC_1;
    header[2] = cmd;
    header[3] = (uint8_t)(len & 0xFFU);
    header[4] = (uint8_t)((len >> 8) & 0xFFU);
    header[5] = (uint8_t)(seq & 0xFFU);
    header[6] = (uint8_t)((seq >> 8) & 0xFFU);

    /* The CRC covers CMD, LENGTH, SEQ and DATA -- not the MAGIC, which
       validates itself simply by being recognized. */
    crc32_reset();
    crc32_update(&header[FRAME_OFF_CMD], FRAME_HEADER_SIZE - FRAME_OFF_CMD);
    if (len > 0U && data != 0) {
        crc32_update(data, len);
    }
    uint32_t crc = crc32_get();

    uint8_t trailer[FRAME_CRC_SIZE];
    trailer[0] = (uint8_t)(crc & 0xFFU);
    trailer[1] = (uint8_t)((crc >> 8) & 0xFFU);
    trailer[2] = (uint8_t)((crc >> 16) & 0xFFU);
    trailer[3] = (uint8_t)((crc >> 24) & 0xFFU);

    uart_write(&uart_proto, header, FRAME_HEADER_SIZE);
    if (len > 0U && data != 0) {
        uart_write(&uart_proto, data, len);
    }
    uart_write(&uart_proto, trailer, FRAME_CRC_SIZE);
}


static void send_ack(uint16_t seq)
{
    send_frame(RSP_ACK, seq, 0, 0);
}


static void send_nack(uint16_t seq, uint8_t err)
{
    send_frame(RSP_NACK, seq, &err, 1);
}


/* ----------------------------------------------------------------
 * Transfer abort
 * ---------------------------------------------------------------- */

static void abort_transfer(void)
{
    state = PROTO_IDLE;
    bytes_written = 0;
    expected_seq = 0;
    last_seq = 0xFFFFU;
}


/* ----------------------------------------------------------------
 * Command handlers
 * ---------------------------------------------------------------- */

static void on_get_info(uint16_t seq)
{
    metadata_t meta;
    uint8_t  active = SLOT_A;
    uint8_t  st     = STATE_EMPTY;
    uint32_t ver    = 0;

    if (metadata_read(&meta)) {
        active = meta.active_slot;
        st     = meta.slot[active].state;
        ver    = meta.slot[active].version;
    }

    /* The response is built byte by byte rather than by casting a
       struct: it crosses a serial link, so its layout must be
       explicit and compiler-independent. */
    uint8_t payload[12];

    payload[0]  = (uint8_t)(ver & 0xFFU);
    payload[1]  = (uint8_t)((ver >> 8) & 0xFFU);
    payload[2]  = (uint8_t)((ver >> 16) & 0xFFU);
    payload[3]  = (uint8_t)((ver >> 24) & 0xFFU);

    payload[4]  = (uint8_t)(BOOTLOADER_VERSION & 0xFFU);
    payload[5]  = (uint8_t)((BOOTLOADER_VERSION >> 8) & 0xFFU);
    payload[6]  = (uint8_t)((BOOTLOADER_VERSION >> 16) & 0xFFU);
    payload[7]  = (uint8_t)((BOOTLOADER_VERSION >> 24) & 0xFFU);

    payload[8]  = (uint8_t)PROTO_VERSION;
    payload[9]  = active;
    payload[10] = (uint8_t)(1U - active);    /* free slot */
    payload[11] = st;

    send_frame(RSP_INFO, seq, payload, 12);
}


static void on_start_update(uint16_t seq, const uint8_t *data, uint16_t len)
{
    if (len != 16U) {
        send_nack(seq, ERR_LENGTH);
        return;
    }

    uint32_t size = (uint32_t)data[0]
                  | ((uint32_t)data[1] << 8)
                  | ((uint32_t)data[2] << 16)
                  | ((uint32_t)data[3] << 24);

    uint32_t crc  = (uint32_t)data[4]
                  | ((uint32_t)data[5] << 8)
                  | ((uint32_t)data[6] << 16)
                  | ((uint32_t)data[7] << 24);

    uint32_t ver  = (uint32_t)data[8]
                  | ((uint32_t)data[9] << 8)
                  | ((uint32_t)data[10] << 16)
                  | ((uint32_t)data[11] << 24);

    uint8_t slot  = data[12];
    uint8_t proto = data[13];

    if (proto != PROTO_VERSION) {
        send_nack(seq, ERR_PROTO_VER);
        return;
    }

    metadata_t meta;
    uint8_t active = metadata_read(&meta) ? meta.active_slot : SLOT_A;
    uint8_t free   = (uint8_t)(1U - active);

    /* All checks happen BEFORE any erase. A rejection must not
       destroy anything. */
    if (size == 0U || size > SLOT_SIZE) {
        send_nack(seq, ERR_SIZE);
        return;
    }
    if (slot != free) {
        send_nack(seq, ERR_SLOT);
        return;
    }

    /* Fail-safe ordering: mark IN_PROGRESS before any destructive
       operation. The reverse order would leave a window during which
       metadata claims a valid firmware exists while it has just been
       erased. */
    metadata_t updated;
    uint8_t *raw = (uint8_t *)&updated;
    for (unsigned i = 0; i < sizeof(updated); i++) {
        raw[i] = 0;
    }
    /* Preserve the current state: the OTHER slot's information must
       survive, otherwise a later rollback would no longer be able to
       describe the fallback image.

       boot_fail_count is preserved for the same reason. It describes
       the slot currently on trial, which is not the one being written
       here, and a transfer that starts is not evidence that that slot
       works. Clearing it here would let a host that merely BEGINS an
       update -- and then fails, or is interrupted -- reset the trial
       counter of a firmware that is busy failing, pushing rollback
       further away every time. on_end_update() clears it once the new
       image is actually in flash, which is the point at which the
       counter genuinely no longer describes anything. */
    metadata_t current;
    if (metadata_read(&current)) {
        updated = current;
    }
    updated.active_slot        = active;   /* unchanged until validated */
    updated.slot[slot].size    = size;
    updated.slot[slot].crc32   = crc;
    updated.slot[slot].version = ver;
    updated.slot[slot].state   = STATE_IN_PROGRESS;

    if (metadata_write(&updated) != META_OK) {
        send_nack(seq, ERR_FLASH);
        return;
    }

    /* The slot is not erased here: each page will be erased just
       before being written. Erasing 480 KB upfront would cost nearly
       five seconds for a firmware that may only occupy 20 KB. */

    target_slot   = slot;
    fw_size       = size;
    fw_crc32      = crc;
    fw_version    = ver;
    bytes_written = 0;
    expected_seq  = (uint16_t)(seq + 1U);
    last_seq      = seq;
    state         = PROTO_RECEIVING;
    timeout_ms    = TIMEOUT_DATA_MS;

    send_ack(seq);
}


static void on_data(uint16_t seq, const uint8_t *data, uint16_t len)
{
    if (state != PROTO_RECEIVING) {
        send_nack(seq, ERR_STATE);
        return;
    }

    /* Retransmission: the PC did not receive the previous ACK. It
       does not expect a re-write -- flash cannot be reprogrammed
       without erasing -- but the missing acknowledgement. Reprocessing
       the frame must have no side effect. */
    if (seq == last_seq) {
        send_ack(seq);
        return;
    }

    if (seq != expected_seq) {
        send_nack(seq, ERR_SEQ);
        abort_transfer();
        return;
    }

    if (len == 0U || (len % 8U) != 0U) {
        /* Flash only programs in double-words. A length that is not
           a multiple of 8 cannot be written as-is. */
        send_nack(seq, ERR_LENGTH);
        return;
    }

    uint32_t offset = bytes_written;

    if (offset + len > fw_size) {
        send_nack(seq, ERR_SIZE);
        abort_transfer();
        return;
    }

    uint32_t addr = SLOT_ADDR(target_slot) + offset;

    /* Lazy erase. The block size divides the page size exactly, so a
       page boundary always coincides with a block boundary: no block
       ever straddles two pages. */
    if ((offset % FLASH_PAGE_SIZE) == 0U) {
        if (flash_erase_page(addr) != FLASH_OK) {
            send_nack(seq, ERR_FLASH);
            abort_transfer();
            return;
        }
    }

    if (flash_write(addr, data, len) != FLASH_OK) {
        send_nack(seq, ERR_FLASH);
        abort_transfer();
        return;
    }

    bytes_written += len;
    last_seq      = seq;
    expected_seq  = (uint16_t)(seq + 1U);

    send_ack(seq);
}


static void on_end_update(uint16_t seq)
{
    if (state != PROTO_RECEIVING) {
        send_nack(seq, ERR_STATE);
        return;
    }

    if (bytes_written != fw_size) {
        send_nack(seq, ERR_SIZE);
        abort_transfer();
        return;
    }

    /* Global verification by re-reading flash.
     *
     * The per-frame CRC validated delivery; this one validates storage.
     * It detects what the former cannot: a defective cell, a partial
     * write, an addressing error. */
    const uint8_t *stored = (const uint8_t *)SLOT_ADDR(target_slot);
    uint32_t computed = crc32_compute(stored, fw_size);

    if (computed != fw_crc32) {
        send_nack(seq, ERR_GLOBAL_CRC);
        abort_transfer();
        return;
    }

    /* STATE_TESTING, not STATE_VALID: a correct CRC proves the
       integrity of the image, not that it works correctly. A firmware
       transferred without any corruption can crash within its first
       second. The application must confirm itself. */
    metadata_t meta;
    uint8_t *raw = (uint8_t *)&meta;
    for (unsigned i = 0; i < sizeof(meta); i++) {
        raw[i] = 0;
    }
    metadata_t current2;
    if (metadata_read(&current2)) {
        meta = current2;
    }
    meta.active_slot                  = target_slot;
    meta.boot_fail_count              = 0;
    meta.slot[target_slot].size       = fw_size;
    meta.slot[target_slot].crc32      = fw_crc32;
    meta.slot[target_slot].version    = fw_version;
    meta.slot[target_slot].state      = STATE_TESTING;

    if (metadata_write(&meta) != META_OK) {
        send_nack(seq, ERR_FLASH);
        abort_transfer();
        return;
    }

    send_ack(seq);
    uart_flush(&uart_proto);

    state = PROTO_COMPLETE;
}


static void dispatch(uint8_t cmd, uint16_t seq,
                     const uint8_t *data, uint16_t len)
{
    cnt_frames_ok++;

    switch (cmd) {
    case CMD_GET_INFO:
        on_get_info(seq);
        break;
    case CMD_START_UPDATE:
        on_start_update(seq, data, len);
        break;
    case CMD_DATA:
        on_data(seq, data, len);
        break;
    case CMD_END_UPDATE:
        on_end_update(seq);
        break;
    case CMD_ABORT:
        abort_transfer();
        send_ack(seq);
        break;
    default:
        send_nack(seq, ERR_STATE);
        break;
    }
}


/* ----------------------------------------------------------------
 * Byte assembly
 * ---------------------------------------------------------------- */

static void rx_reset(void)
{
    rx_state    = RX_MAGIC0;
    rx_index    = 0;
    rx_expected = 0;
}


static void feed(uint8_t byte)
{
    switch (rx_state) {

    case RX_MAGIC0:
        if (byte == FRAME_MAGIC_0) {
            rx_frame[0] = byte;
            rx_state = RX_MAGIC1;
        }
        /* Any other byte is ignored: looking for a frame start
           in a potentially noisy stream. */
        break;

    case RX_MAGIC1:
        if (byte == FRAME_MAGIC_1) {
            rx_frame[1] = byte;
            rx_index    = 2;
            rx_expected = FRAME_HEADER_SIZE - 2U;
            rx_state    = RX_HEADER;
        } else if (byte == FRAME_MAGIC_0) {
            /* Sequence 0xAA 0xAA 0x55: the second 0xAA may be the
               real start. Stay waiting for 0x55. */
            rx_frame[0] = byte;
        } else {
            rx_state = RX_MAGIC0;
        }
        break;

    case RX_HEADER:
        rx_frame[rx_index++] = byte;
        if (--rx_expected == 0U) {
            uint16_t len = (uint16_t)rx_frame[FRAME_OFF_LENGTH]
                         | (uint16_t)((uint16_t)rx_frame[FRAME_OFF_LENGTH + 1] << 8);

            /* Plausibility check BEFORE any buffering. An unbounded
               length field is a classic buffer-overflow vector:
               waiting for 60000 bytes would exceed the frame buffer
               capacity. */
            if (len > MAX_PAYLOAD_SIZE) {
                cnt_frames_ko++;
                rx_reset();
                break;
            }

            if (len == 0U) {
                rx_expected = FRAME_CRC_SIZE;
                rx_state    = RX_CRC;
            } else {
                rx_expected = len;
                rx_state    = RX_DATA;
            }
        }
        break;

    case RX_DATA:
        rx_frame[rx_index++] = byte;
        if (--rx_expected == 0U) {
            rx_expected = FRAME_CRC_SIZE;
            rx_state    = RX_CRC;
        }
        break;

    case RX_CRC:
        rx_frame[rx_index++] = byte;
        if (--rx_expected == 0U) {
            uint16_t len = (uint16_t)rx_frame[FRAME_OFF_LENGTH]
                         | (uint16_t)((uint16_t)rx_frame[FRAME_OFF_LENGTH + 1] << 8);

            uint32_t off = FRAME_HEADER_SIZE + len;

            uint32_t received = (uint32_t)rx_frame[off]
                              | ((uint32_t)rx_frame[off + 1] << 8)
                              | ((uint32_t)rx_frame[off + 2] << 16)
                              | ((uint32_t)rx_frame[off + 3] << 24);

            uint32_t computed = crc32_compute(&rx_frame[FRAME_OFF_CMD],
                                             (FRAME_HEADER_SIZE - FRAME_OFF_CMD) + len);

            if (computed == received) {
                uint8_t  cmd = rx_frame[FRAME_OFF_CMD];
                uint16_t seq = (uint16_t)rx_frame[FRAME_OFF_SEQ]
                             | (uint16_t)((uint16_t)rx_frame[FRAME_OFF_SEQ + 1] << 8);

                dispatch(cmd, seq, &rx_frame[FRAME_OFF_DATA], len);
            } else {
                cnt_frames_ko++;
                uint16_t seq = (uint16_t)rx_frame[FRAME_OFF_SEQ]
                             | (uint16_t)((uint16_t)rx_frame[FRAME_OFF_SEQ + 1] << 8);
                send_nack(seq, ERR_CRC);
            }

            rx_reset();
        }
        break;

    default:
        rx_reset();
        break;
    }
}


/* ----------------------------------------------------------------
 * Public interface
 * ---------------------------------------------------------------- */

void protocol_init(void)
{
    rx_reset();
    state = PROTO_IDLE;

    expected_seq  = 0;
    last_seq      = 0xFFFFU;
    target_slot   = SLOT_A;
    fw_size       = 0;
    fw_crc32      = 0;
    fw_version    = 0;
    bytes_written = 0;

    last_activity_ms = 0;
    timeout_ms       = TIMEOUT_START_MS;

    cnt_frames_ok = 0;
    cnt_frames_ko = 0;
}


void protocol_poll(uint32_t now_ms)
{
    uint8_t byte;
    int received = 0;

    /* Bounded drain.
     *
     * This loop used to run until the buffer emptied, with the
     * watchdog refresh sitting outside it in update_mode(). It
     * terminates only because the CPU consumes bytes faster than
     * 115200 baud delivers them -- a statement about relative speeds,
     * not a property of the code, and one that stops being true the
     * moment the clock changes or the link speeds up.
     *
     * app/main.c's trigger poll already bounds its own drain and says
     * in as many words that the loop's exit should not depend on the
     * CPU outpacing the line. The same standard belongs here, in the
     * loop that actually matters.
     *
     * One bufferful is the natural bound: no more than that can be
     * pending, so a full pass still drains everything available, and
     * the caller reaches iwdg_feed() every iteration no matter what
     * the host does. */
    uint32_t budget = UART_RX_BUFFER_SIZE;

    while (budget-- > 0U && uart_getc(&uart_proto, &byte)) {
        feed(byte);
        received = 1;
    }

    /* The inactivity timer resets after processing all available bytes,
       never on receipt of a single one. A flash write lasting several
       tens of milliseconds must not be counted as a silence period. */
    if (received) {
        last_activity_ms = now_ms;
        return;
    }

    if (state == PROTO_RECEIVING) {
        if ((now_ms - last_activity_ms) > timeout_ms) {
            /* The sender has gone silent. The target slot contains a
               partial firmware; metadata remains IN_PROGRESS, which
               the next boot will know how to interpret. */
            abort_transfer();
            last_activity_ms = now_ms;
        }
    }
}


protocol_state_t protocol_get_state(void)
{
    return state;
}


int protocol_update_complete(void)
{
    return (state == PROTO_COMPLETE);
}


uint32_t protocol_frames_received(void) { return cnt_frames_ok; }
uint32_t protocol_frames_rejected(void) { return cnt_frames_ko; }
uint32_t protocol_bytes_written(void)   { return bytes_written; }
