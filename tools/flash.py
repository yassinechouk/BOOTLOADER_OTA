"""
Firmware update tool over serial link.

    ./flash.py --port /dev/ttyACM0 --dir build/
    ./flash.py --port /dev/ttyACM0 --info
    ./flash.py --port /dev/ttyACM0 --file app_slotB.bin --slot B

The binary sent depends on the free slot announced by the board: it
is the board that holds the state, not the tool. In --dir mode, the
file is chosen automatically from app_slotA.bin and app_slotB.bin.

This choice follows from the dual-slot architecture: the application
is linked to a fixed address, so compiled twice, once per slot. See
PROTOCOL.md for the discarded alternatives.
"""

import argparse
import os
import sys
import time

import protocol as p
from crc32 import crc32_stm32
from transport import (SerialTransport, TcpTransport,
                       TransportError, Timeout, Disconnected)


# ---------------------------------------------------------------
# Display
# ---------------------------------------------------------------
class Term:
    GREY   = "\033[90m"
    GREEN  = "\033[92m"
    RED    = "\033[91m"
    YELLOW = "\033[93m"
    BLUE   = "\033[94m"
    BOLD   = "\033[1m"
    END    = "\033[0m"

    active = sys.stdout.isatty()

    @classmethod
    def c(cls, text, color):
        return f"{color}{text}{cls.END}" if cls.active else text


def info(msg):    print(f"  {msg}")
def success(msg): print(f"  {Term.c('OK', Term.GREEN)}    {msg}")
def failure(msg): print(f"  {Term.c('FAILED', Term.RED)} {msg}")
def step(msg):    print(f"\n{Term.c(msg, Term.BOLD)}")


def progress_bar(current, total, width=40):
    frac = current / total if total else 1.0
    filled = int(frac * width)
    bar = "#" * filled + "-" * (width - filled)
    pct = int(frac * 100)
    sys.stdout.write(f"\r  [{bar}] {pct:3}%  {current}/{total} bytes")
    sys.stdout.flush()


# Mirror of shared/metadata.h -- only what the estimate needs.
FLASH_PAGE_SIZE = 2048


def estimated_duration(size_bytes, baudrate=115200):
    """
    Rough transfer time. Each byte occupies 10 bits: start + 8 + stop.

    Counting only the outgoing payload under-predicts by about half,
    which is worse than not predicting at all. Three things it missed:
    every block is answered by an ACK frame travelling the other way,
    the board is not listening while it programs, and a 2 KB page has
    to be erased every eighth block.

    The flash constants are fitted to measurements on the STM32L476 at
    115200 baud. This is an estimate, not a derivation.
    """
    blocks = (size_bytes + p.DATA_BLOCK_SIZE - 1) // p.DATA_BLOCK_SIZE

    on_wire = (size_bytes
               + blocks * p.FRAME_OVERHEAD      # outgoing framing
               + blocks * p.FRAME_OVERHEAD      # ACK coming back
               + 6 * p.FRAME_OVERHEAD + 28)     # GET_INFO/START/END
    serial_s = on_wire * 10 / baudrate

    pages = (size_bytes + FLASH_PAGE_SIZE - 1) // FLASH_PAGE_SIZE
    flash_s = pages * 0.022 + (size_bytes / 8) * 0.00028

    return serial_s + flash_s


def next_version(current: int) -> int:
    """
    One patch above whatever the board reports it is running.

    The version field exists to tell two images apart. A fixed default
    meant every image flashed during development stamped itself with
    the same number, so the field said nothing -- and both slots ended
    up claiming v0.1.0, which is exactly the moment you want to know
    which is which.

    Deriving it from GET_INFO keeps versions distinct and ordered with
    nothing to remember. It also puts the question on the side of the
    link that actually knows the answer, the same reasoning that leaves
    slot selection to the bootloader.
    """
    major = (current >> 16) & 0xFF
    minor = (current >> 8) & 0xFF
    patch = (current & 0xFF) + 1

    if patch > 0xFF:
        patch, minor = 0, minor + 1
    if minor > 0xFF:
        minor, major = 0, (major + 1) & 0xFF

    return (major << 16) | (minor << 8) | patch


# ---------------------------------------------------------------
# Operations
# ---------------------------------------------------------------
def read_info(tr) -> p.InfoResponse:
    rep = tr.exchange(p.Frame(p.CMD_GET_INFO, 0))

    if rep.cmd == p.RSP_NACK:
        code = rep.data[0] if rep.data else 0
        raise TransportError(f"GET_INFO rejected: {p.ERROR_NAMES.get(code, code)}")

    if rep.cmd != p.RSP_INFO:
        raise TransportError(f"unexpected response: {rep}")

    return p.InfoResponse.unpack(rep.data)


def print_info(nfo: p.InfoResponse):
    def version(v):
        return f"{(v >> 16) & 0xFF}.{(v >> 8) & 0xFF}.{v & 0xFF}"

    info(f"protocol       : v{nfo.proto_version}")
    info(f"bootloader     : v{version(nfo.bl_version)}")
    info(f"active firmware: v{version(nfo.fw_version)}")
    info(f"active slot    : {'AB'[nfo.active_slot]}")
    info(f"free slot      : {Term.c('AB'[nfo.free_slot], Term.BLUE)}")
    info(f"state          : {p.STATE_NAMES.get(nfo.state, nfo.state)}")


def choose_binary(directory: str, slot: int) -> str:
    """
    Selects the binary corresponding to the free slot.

    The firmware is linked to a fixed address: app_slotA.bin only
    works at slot A's address. Sending the wrong one would produce
    an image with a perfectly valid CRC but that cannot execute —
    a case the bootloader catches via the TESTING state, but avoided here.
    """
    name = f"app_slot{'AB'[slot]}.bin"
    path = os.path.join(directory, name)

    if not os.path.isfile(path):
        raise SystemExit(
            f"{path} not found.\n"
            f"The dual-slot architecture requires two binaries, one per\n"
            f"slot. Check that the application Makefile produces\n"
            f"app_slotA.bin and app_slotB.bin."
        )
    return path


def send_firmware(tr, firmware: bytes, slot: int, version: int,
                  baudrate: int) -> bool:
    crc = crc32_stm32(firmware)
    blocks = list(p.split_firmware(firmware))

    step("Transfer")
    info(f"size     : {len(firmware)} bytes")
    info(f"CRC32    : 0x{crc:08X}")
    info(f"blocks   : {len(blocks)} x {p.DATA_BLOCK_SIZE}")
    info(f"target   : slot {'AB'[slot]}")
    info(f"version  : v{(version >> 16) & 0xFF}."
         f"{(version >> 8) & 0xFF}.{version & 0xFF}")
    info(f"estimate : {estimated_duration(len(firmware), baudrate):.1f} s")
    print()

    # --- announce ---
    su = p.StartUpdate(len(firmware), crc, version, slot)
    rep = tr.exchange(p.Frame(p.CMD_START_UPDATE, 0, su.pack()))

    if rep.cmd == p.RSP_NACK:
        code = rep.data[0] if rep.data else 0
        failure(f"transfer rejected: {p.ERROR_NAMES.get(code, code)}")
        if code == p.ERR_SLOT:
            info("the binary does not match the slot announced by the board")
        elif code == p.ERR_SIZE:
            info("firmware too large for the slot")
        return False

    # --- blocks ---
    start = time.time()
    sent = 0
    seq = 1

    for block in blocks:
        try:
            rep = tr.exchange(p.Frame(p.CMD_DATA, seq, block))
        except Disconnected:
            print()
            raise
        except TransportError as e:
            print()
            failure(f"block {seq}/{len(blocks)}: {e}")
            info(f"{sent} bytes transmitted before failure")
            return False

        if rep.cmd == p.RSP_NACK:
            print()
            code = rep.data[0] if rep.data else 0
            failure(f"block {seq} rejected: {p.ERROR_NAMES.get(code, code)}")
            return False

        sent += len(block)
        seq += 1
        progress_bar(sent, len(firmware))

    print()
    elapsed = time.time() - start
    throughput = len(firmware) / elapsed if elapsed > 0 else 0
    info(f"transmitted in {elapsed:.1f} s ({throughput:.0f} B/s)")

    # --- finalise ---
    step("Verification")
    info("re-reading flash and computing global CRC...")

    try:
        rep = tr.exchange(p.Frame(p.CMD_END_UPDATE, seq), retries=1)
    except Timeout:
        # Verification re-reads the entire slot; on a large firmware
        # this exceeds the ordinary timeout.
        rep = tr.receive(timeout=10.0)

    if rep.cmd == p.RSP_NACK:
        code = rep.data[0] if rep.data else 0
        failure(f"verification failed: {p.ERROR_NAMES.get(code, code)}")
        if code == p.ERR_GLOBAL_CRC:
            info("content read back differs from firmware sent")
        return False

    success("global CRC verified")
    success("image marked TESTING")
    info("board rebooting; application must confirm itself")
    return True


# ---------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(
        description="Firmware update over serial link",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)

    ap.add_argument("--port", default="/dev/ttyACM0",
                    help="serial device (ignored when --host is given)")
    ap.add_argument("--host",
                    help="WiFi gateway address, e.g. 192.168.4.1; "
                         "uses TCP instead of the serial port")
    ap.add_argument("--tcp-port", type=int, default=3333,
                    help="gateway TCP port (default 3333)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--dir", help="directory containing app_slotA.bin and app_slotB.bin")
    ap.add_argument("--file", help="explicit binary")
    ap.add_argument("--slot", choices=["A", "B"],
                    help="force target slot (with --file)")
    ap.add_argument("--version", default=None,
                    help="firmware version M.m.p "
                         "(default: one patch above the board's)")
    ap.add_argument("--info", action="store_true",
                    help="query the board without sending anything")
    ap.add_argument("--verbose", action="store_true")

    args = ap.parse_args()

    if not args.info and not args.dir and not args.file:
        ap.error("specify --dir, --file or --info")

    # Validated here so a typo fails before touching the board; the
    # value is only resolved once GET_INFO has said what is installed.
    explicit_version = None
    if args.version is not None:
        try:
            maj, mnr, pch = (int(x) for x in args.version.split("."))
        except ValueError:
            ap.error("version expected in M.m.p format")
        explicit_version = (maj << 16) | (mnr << 8) | pch

    print(Term.c("\nFirmware update", Term.BOLD))
    if args.host:
        print(f"  gateway {args.host}:{args.tcp_port} over WiFi")
    else:
        print(f"  port {args.port} @ {args.baud} baud")

    # A diagnostic message must describe the real state of the board.
    # Without this flag, a disconnection that occurred BEFORE the
    # transfer would warn about a partial image that does not exist.
    transfer_started = False

    try:
        if args.host:
            link = TcpTransport(args.host, args.tcp_port, verbose=args.verbose)
        else:
            link = SerialTransport(args.port, args.baud, verbose=args.verbose)
        with link as tr:

            step("Board status")
            nfo = read_info(tr)
            print_info(nfo)

            if nfo.proto_version != p.PROTO_VERSION:
                failure(f"protocol v{nfo.proto_version} on board, "
                        f"v{p.PROTO_VERSION} in tool")
                return 1

            if args.info:
                print()
                return 0

            version = (explicit_version if explicit_version is not None
                       else next_version(nfo.fw_version))

            # --- choose binary ---
            if args.file:
                path = args.file
                slot = nfo.free_slot
                if args.slot:
                    slot = 0 if args.slot == "A" else 1
                    if slot != nfo.free_slot:
                        failure(f"board expects slot "
                                f"{'AB'[nfo.free_slot]}, not {args.slot}")
                        return 1
            else:
                path = choose_binary(args.dir, nfo.free_slot)
                slot = nfo.free_slot

            with open(path, "rb") as f:
                firmware = f.read()

            if not firmware:
                failure(f"{path} is empty")
                return 1

            info(f"file           : {os.path.basename(path)}")

            # --- alignment ---
            remainder = len(firmware) % 8
            if remainder:
                # Flash only programs in double-words. A non-aligned
                # firmware is padded with 0xFF, the value of an erased
                # cell, which is therefore neutral.
                padding = 8 - remainder
                firmware += b"\xFF" * padding
                info(f"padded by {padding} bytes (64-bit alignment)")

            transfer_started = True

            if not send_firmware(tr, firmware, slot, version, args.baud):
                print()
                return 1

            print()
            return 0

    except KeyboardInterrupt:
        print("\n\n  interrupted")
        return 130

    except Disconnected as e:
        print()
        failure(str(e))
        if transfer_started:
            info("the target slot contains a partial image; its")
            info("metadata remains IN_PROGRESS, which the bootloader")
            info("will interpret correctly at next boot.")
            info("The active slot was not touched: the board boots")
            info("normally on its previous firmware.")
        else:
            info("no transfer had started: the board is intact")
            info("check the cable, reconnect, then retry")
        return 1

    except TransportError as e:
        print()
        failure(str(e))
        info("check that the board is in receive mode "
             "and that no terminal is holding the port")
        info("the bootloader's listen window only lasts 2 s "
             "after reset")
        return 1


if __name__ == "__main__":
    sys.exit(main())
