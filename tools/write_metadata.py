#!/usr/bin/env python3
"""
Write bootloader metadata directly over SWD.

Why this exists
---------------
The bootloader refuses to jump to an image whose CRC does not match the
size and checksum recorded in metadata. That is the point of the
check -- but it means an image flashed over SWD never boots, because
SWD writes the slot and not the record describing it. Only a completed
OTA writes both.

Since the protocol moved to USART1 for the gateway, an OTA needs either
the ESP32 or a USB-TTL adapter on PA9/PA10. Without one of those there
was no way to bring a board up from a bare SWD probe at all.

This closes that gap: it computes the image CRC exactly as the
bootloader will, builds the 48-byte record, and writes it through
OpenOCD.

Layout is a binary contract
---------------------------
The struct below mirrors shared/metadata.h field for field. Getting a
field width wrong here does not fail loudly: it shifts everything after
it, the CRC still validates because it is computed over whatever was
produced, and the board simply refuses to boot for a reason that points
nowhere. The format string and the header must be read together.

    slot_info_t   size u32 | crc32 u32 | version u32 | state u8 | rsv[3]
    metadata_t    magic u32 | counter u32 | slot[2] | active u8
                  | boot_fail u8 | rsv[2] | meta_crc32 u32

Usage
-----
    ./write_metadata.py --slot A --image ../app/app_slotA.bin
    ./write_metadata.py --slot B --image ../app/app_slotB.bin --version 1.0.0
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile

from crc32 import crc32_stm32

METADATA_MAGIC   = 0x424C4D44          # "BLMD"
META_PAGE_A_ADDR = 0x080FF000
META_PAGE_B_ADDR = 0x080FF800
SLOT_ADDR        = {0: 0x08008000, 1: 0x08080000}
SLOT_SIZE        = 480 * 1024
STATE_EMPTY      = 0x00
STATE_VALID      = 0x03

SLOT_FMT = "<IIIB3s"                   # 16 bytes
META_FMT = "<II" + "IIIB3s" * 2 + "BB2sI"   # 48 bytes

OPENOCD_IFACE  = "interface/stlink.cfg"
OPENOCD_TARGET = "target/stm32l4x.cfg"


def build_metadata(active, images, counter=1):
    """
    images: {slot_index: (size, crc32, version)} -- absent slots are EMPTY.
    """
    fields = [METADATA_MAGIC, counter]

    for slot in (0, 1):
        if slot in images:
            size, crc, ver = images[slot]
            fields += [size, crc, ver, STATE_VALID, b"\x00" * 3]
        else:
            fields += [0, 0, 0, STATE_EMPTY, b"\x00" * 3]

    fields += [active, 0, b"\x00" * 2]

    # The CRC covers everything except itself: the first 44 bytes.
    head = struct.pack(META_FMT[:-1], *fields)
    if len(head) != 44:
        raise SystemExit(f"internal: header is {len(head)} bytes, expected 44")

    return head + struct.pack("<I", crc32_stm32(head))


def read_existing():
    """
    Read both metadata pages off the board and return the authoritative
    record, or None if neither is usable.

    Preserving what is already there is the whole point. Describing only
    the slot being written, as the first version of this tool did, sets
    the other one to EMPTY -- which silently removes the fallback image
    and leaves the board with nothing to roll back to. The bootloader
    would still boot, so the damage stays invisible until the day it
    matters.

    Selection follows drivers/metadata_mgr.c: a copy counts only if its
    magic and CRC are both right, and the higher counter wins.
    """
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        path = f.name
    try:
        ok, _ = openocd([
            "init", "halt",
            f"dump_image {path} {META_PAGE_A_ADDR} 48",
            "exit",
        ])
        page_a = open(path, "rb").read() if ok else b""

        ok, _ = openocd([
            "init", "halt",
            f"dump_image {path} {META_PAGE_B_ADDR} 48",
            "exit",
        ])
        page_b = open(path, "rb").read() if ok else b""
    finally:
        os.unlink(path)

    best = None
    for raw in (page_a, page_b):
        if len(raw) != 48:
            continue
        fields = struct.unpack(META_FMT, raw)
        if fields[0] != METADATA_MAGIC:
            continue
        if crc32_stm32(raw[:44]) != fields[15]:
            continue
        if best is None or fields[1] > best[1]:
            best = fields
    return best


def openocd(commands):
    cmd = ["openocd", "-f", OPENOCD_IFACE, "-f", OPENOCD_TARGET]
    for c in commands:
        cmd += ["-c", c]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=90)
    return r.returncode == 0, r.stdout + r.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slot", choices=["A", "B"], required=True,
                    help="slot the image was flashed into")
    ap.add_argument("--image", required=True, help="the .bin written to that slot")
    ap.add_argument("--version", default="1.0.0")
    ap.add_argument("--dry-run", action="store_true",
                    help="build and print the record, touch nothing")
    args = ap.parse_args()

    slot = 0 if args.slot == "A" else 1

    data = open(args.image, "rb").read()
    # The bootloader reads whole double-words; pad as the host tool does.
    if len(data) % 8:
        data += b"\xFF" * (8 - len(data) % 8)

    if not data or len(data) > SLOT_SIZE:
        raise SystemExit(f"image is {len(data)} bytes, slot holds {SLOT_SIZE}")

    crc = crc32_stm32(data)
    parts = [int(p) for p in args.version.split(".")]
    while len(parts) < 3:
        parts.append(0)
    version = (parts[0] << 16) | (parts[1] << 8) | parts[2]

    # Preserve whatever the other slot already holds, and continue the
    # counter so this record wins the comparison against both pages.
    images  = {slot: (len(data), crc, version)}
    counter = 1
    existing = read_existing()
    if existing:
        counter = existing[1] + 1
        other = 1 - slot
        base = 2 + other * 5          # slot[other] starts here in the tuple
        if existing[base + 3] != STATE_EMPTY and existing[base] != 0:
            images[other] = (existing[base], existing[base + 1], existing[base + 2])
            print(f"  preserving slot {'AB'[other]}: "
                  f"{existing[base]} bytes, crc 0x{existing[base + 1]:08X}")
        else:
            print(f"  slot {'AB'[other]} was already empty")
    else:
        print("  no usable existing record; the other slot will read EMPTY")

    record = build_metadata(slot, images, counter)
    if len(record) != 48:
        raise SystemExit(f"record is {len(record)} bytes, expected 48")

    print(f"  slot     : {args.slot} @ 0x{SLOT_ADDR[slot]:08X}")
    print(f"  image    : {args.image}")
    print(f"  size     : {len(data)} bytes (padded to a multiple of 8)")
    print(f"  CRC32    : 0x{crc:08X}")
    print(f"  version  : 0x{version:08X}")
    print(f"  record   : {record.hex(' ')}")

    if args.dry_run:
        print("  dry run: nothing written")
        return 0

    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        f.write(record)
        path = f.name

    try:
        # Both pages are erased so no stale copy with a higher counter
        # can win the comparison, then one fresh record is written.
        ok, out = openocd([
            "init", "halt",
            f"flash erase_address {META_PAGE_A_ADDR} 4096",
            f"flash write_bank 0 {path} {META_PAGE_A_ADDR - 0x08000000}",
            "reset run", "exit",
        ])
    finally:
        os.unlink(path)

    if not ok:
        print("  FAILED -- openocd output:")
        for line in out.splitlines():
            if any(k in line for k in ("Error", "error", "failed")):
                print("    " + line)
        return 1

    print("  written and board reset")
    return 0


if __name__ == "__main__":
    sys.exit(main())
