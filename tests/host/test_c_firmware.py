#!/usr/bin/env python3
"""
Tests the FIRMWARE C, not a reimplementation of it.

Why this exists
---------------
tools/test_protocol.py drives bootloader_sim.py, a Python model. That
model has already drifted: it packs metadata as 28 bytes, the layout
shared/metadata.h documents as a fixed defect, while the firmware uses
48 bytes with per-slot descriptors. Every one of those tests passes
against a structure the board no longer uses.

This file loads metadata_mgr.c and protocol_mgr.c themselves, compiled
unmodified, and drives them the way the board does: bytes in, bytes
out. A failure here is a firmware failure.

What is covered, and what is not
--------------------------------
Covered: the protocol state machine, sequencing, retransmission,
metadata A/B selection and its CRC, the boot-critical erase-before-
write ordering.

Not covered: flash.c, crc.c and uart.c, which are register-level
drivers replaced by models of their documented contracts. The CRC
model is checked against the vectors the header states were measured
on silicon, so a divergence there is caught rather than assumed.
"""

import ctypes
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))

import protocol as p
from crc32 import crc32_stm32

LIB = os.path.join(HERE, "libfirmware.so")

SLOT_A_ADDR = 0x08008000
SLOT_B_ADDR = 0x08080000
META_A_ADDR = 0x080FF000
META_B_ADDR = 0x080FF800

META_FMT = "<II" + "IIIB3s" * 2 + "BB2sI"

passed = failed = 0


def check(label, cond):
    global passed, failed
    if cond:
        passed += 1
        print(f"  ok   {label}")
    else:
        failed += 1
        print(f"  FAIL {label}")


def section(title):
    print(f"\n--- {title} ---")


class Firmware:
    """The real C, behind a byte-level interface."""

    def __init__(self):
        if not os.path.exists(LIB):
            subprocess.run(["make", "-s"], cwd=HERE, check=True)
        self.lib = ctypes.CDLL(LIB)
        self.lib.fw_read_flash.restype = None
        self.lib.fw_metadata_size.restype = ctypes.c_uint32
        self.now = 0
        self.reset()

    def reset(self):
        self.lib.fw_reset()
        self.now = 0

    def reboot(self):
        """Restart the firmware WITHOUT clearing flash, as a reset does."""
        self.lib.fw_reboot()

    def exchange(self, frame: p.Frame, advance_ms=10) -> bytes:
        raw = p.encode(frame)
        self.lib.fw_feed(raw, len(raw))
        self.now += advance_ms
        self.lib.fw_poll(self.now)
        buf = (ctypes.c_ubyte * 4096)()
        n = self.lib.fw_take(buf, 4096)
        return bytes(buf[:n])

    def reply(self, frame: p.Frame) -> p.Frame:
        return p.decode(self.exchange(frame))

    def read_flash(self, addr, length) -> bytes:
        buf = (ctypes.c_ubyte * length)()
        self.lib.fw_read_flash(ctypes.c_uint32(addr), buf, length)
        return bytes(buf[:length])

    def write_flash_raw(self, addr, data: bytes):
        self.lib.fw_write_flash_raw(ctypes.c_uint32(addr), data, len(data))

    def metadata(self):
        buf = (ctypes.c_ubyte * 48)()
        if not self.lib.fw_metadata_read(buf):
            return None
        return struct.unpack(META_FMT, bytes(buf))

    @property
    def erase_count(self): return self.lib.fw_erase_count()

    @property
    def state(self): return self.lib.fw_state()


def transfer(fw, image: bytes, slot: int, seq0=0, version=0x010000):
    """A complete update, as flash.py performs it."""
    su = struct.pack("<IIIBBH", len(image), crc32_stm32(image), version, slot,
                     p.PROTO_VERSION, 0)
    rep = fw.reply(p.Frame(p.CMD_START_UPDATE, seq0, su))
    if rep.cmd != p.RSP_ACK:
        return rep

    seq = seq0 + 1
    for off in range(0, len(image), p.DATA_BLOCK_SIZE):
        block = image[off:off + p.DATA_BLOCK_SIZE]
        if len(block) % 8:
            block += b"\xFF" * (8 - len(block) % 8)
        rep = fw.reply(p.Frame(p.CMD_DATA, seq, block))
        if rep.cmd != p.RSP_ACK:
            return rep
        seq += 1

    return fw.reply(p.Frame(p.CMD_END_UPDATE, seq))


def main():
    fw = Firmware()
    print("Firmware C tests (real metadata_mgr.c + protocol_mgr.c)")
    print("=" * 54)

    # ---------------------------------------------------------
    section("Struct layout matches the target")
    check("metadata_t is 48 bytes", fw.lib.fw_metadata_size() == 48)
    check("simulator's 28-byte layout is NOT what the firmware uses",
          fw.lib.fw_metadata_size() != 28)

    # ---------------------------------------------------------
    section("CRC model agrees with the silicon vectors")
    # shared/crc.h records these as measured on an STM32L476RG.
    check('"123456789" -> 0x9B63D02C', crc32_stm32(b"123456789") == 0x9B63D02C)
    check("four zeros -> 0xC704DD7B", crc32_stm32(b"\x00" * 4) == 0xC704DD7B)
    check('"STM32" -> 0xF4F0FF62', crc32_stm32(b"STM32") == 0xF4F0FF62)

    # ---------------------------------------------------------
    section("GET_INFO on a blank board")
    fw.reset()
    rep = fw.reply(p.Frame(p.CMD_GET_INFO, 0))
    check("answers RSP_INFO", rep.cmd == p.RSP_INFO)
    nfo = p.InfoResponse.unpack(rep.data)
    check("protocol version reported", nfo.proto_version == p.PROTO_VERSION)
    check("blank board reports EMPTY", nfo.state == p.STATE_EMPTY)
    check("free slot differs from active", nfo.free_slot != nfo.active_slot)

    # ---------------------------------------------------------
    section("Nominal transfer writes flash and metadata")
    fw.reset()
    image = bytes((i * 7 + 3) & 0xFF for i in range(1000))
    rep = transfer(fw, image, p.SLOT_B)
    check("END_UPDATE acknowledged", rep.cmd == p.RSP_ACK)

    stored = fw.read_flash(SLOT_B_ADDR, len(image))
    check("flash content matches the image", stored == image)

    meta = fw.metadata()
    check("metadata readable after transfer", meta is not None)
    if meta:
        check("active slot switched to B", meta[12] == p.SLOT_B)
        check("state is TESTING, not VALID", meta[7 + 3] == p.STATE_TESTING)
        check("size recorded", meta[7] == len(image))
        check("CRC recorded", meta[8] == crc32_stm32(image))

    # ---------------------------------------------------------
    section("Erase happens lazily, once per page")
    fw.reset()
    before = fw.erase_count
    small = bytes(300)                       # spans one page only
    transfer(fw, small, p.SLOT_B)
    # 300 bytes -> 2 blocks -> 1 page, plus metadata pages.
    check("slot erased at most once for a 300-byte image",
          fw.erase_count - before <= 3)

    # ---------------------------------------------------------
    section("Retransmission is idempotent")
    fw.reset()
    img = bytes(512)
    su = struct.pack("<IIIBBH", len(img), crc32_stm32(img), 0x010000,
                     p.SLOT_B, p.PROTO_VERSION, 0)
    check("START acknowledged", fw.reply(p.Frame(p.CMD_START_UPDATE, 0, su)).cmd == p.RSP_ACK)
    b0 = img[0:256]
    check("first DATA acknowledged", fw.reply(p.Frame(p.CMD_DATA, 1, b0)).cmd == p.RSP_ACK)
    written = fw.lib.fw_bytes_written()
    check("same DATA re-acknowledged", fw.reply(p.Frame(p.CMD_DATA, 1, b0)).cmd == p.RSP_ACK)
    check("retransmission did not advance the offset",
          fw.lib.fw_bytes_written() == written)

    # ---------------------------------------------------------
    section("Out-of-order sequence is rejected")
    fw.reset()
    fw.reply(p.Frame(p.CMD_START_UPDATE, 0, su))
    rep = fw.reply(p.Frame(p.CMD_DATA, 7, b0))
    check("wrong SEQ gets NACK", rep.cmd == p.RSP_NACK)
    check("error code is ERR_SEQ", rep.data[0] == p.ERR_SEQ)

    # ---------------------------------------------------------
    section("Checks run before anything is erased")
    fw.reset()
    huge = struct.pack("<IIIBBH", 600 * 1024, 0, 0x010000, p.SLOT_B,
                       p.PROTO_VERSION, 0)
    before = fw.erase_count
    rep = fw.reply(p.Frame(p.CMD_START_UPDATE, 0, huge))
    check("oversized firmware rejected", rep.cmd == p.RSP_NACK)
    check("error code is ERR_SIZE", rep.data[0] == p.ERR_SIZE)
    check("nothing was erased", fw.erase_count == before)

    wrong = struct.pack("<IIIBBH", 256, 0, 0x010000, p.SLOT_A,
                        p.PROTO_VERSION, 0)
    rep = fw.reply(p.Frame(p.CMD_START_UPDATE, 0, wrong))
    check("wrong target slot rejected", rep.cmd == p.RSP_NACK)
    check("error code is ERR_SLOT", rep.data[0] == p.ERR_SLOT)

    # ---------------------------------------------------------
    section("DATA before START is refused")
    fw.reset()
    rep = fw.reply(p.Frame(p.CMD_DATA, 1, b0))
    check("DATA without START gets NACK", rep.cmd == p.RSP_NACK)
    check("error code is ERR_STATE", rep.data[0] == p.ERR_STATE)

    # ---------------------------------------------------------
    section("Bad global CRC leaves the slot unusable")
    fw.reset()
    img = bytes(256)
    bad = struct.pack("<IIIBBH", len(img), crc32_stm32(img) ^ 0xFFFF,
                      0x010000, p.SLOT_B, p.PROTO_VERSION, 0)
    fw.reply(p.Frame(p.CMD_START_UPDATE, 0, bad))
    fw.reply(p.Frame(p.CMD_DATA, 1, img))
    rep = fw.reply(p.Frame(p.CMD_END_UPDATE, 2))
    check("mismatch detected", rep.cmd == p.RSP_NACK)
    check("error code is ERR_GLOBAL_CRC", rep.data[0] == p.ERR_GLOBAL_CRC)
    meta = fw.metadata()
    check("slot left IN_PROGRESS, not TESTING",
          meta is not None and meta[7 + 3] == p.STATE_IN_PROGRESS)

    # ---------------------------------------------------------
    section("Metadata A/B alternation and CRC")
    fw.reset()
    check("blank board has no metadata", fw.metadata() is None)

    blank = bytes(48)
    fw.lib.fw_metadata_write(blank)
    m1 = fw.metadata()
    check("first write readable", m1 is not None)
    fw.lib.fw_metadata_write(blank)
    m2 = fw.metadata()
    check("counter incremented", m2[1] == m1[1] + 1)

    pa = fw.read_flash(META_A_ADDR, 48)
    pb = fw.read_flash(META_B_ADDR, 48)
    check("both pages hold a record", pa[:4] != b"\xFF" * 4 and pb[:4] != b"\xFF" * 4)
    check("the pages differ", pa != pb)

    # ---------------------------------------------------------
    section("Corrupted metadata is rejected")
    fw.reset()
    fw.lib.fw_metadata_write(blank)
    good = fw.read_flash(META_A_ADDR, 48)
    fw.write_flash_raw(META_A_ADDR, bytes([good[0] ^ 0xFF]) + good[1:])
    fw.write_flash_raw(META_B_ADDR, b"\xFF" * 48)
    check("a record with a broken magic is not accepted", fw.metadata() is None)

    fw.reset()
    fw.lib.fw_metadata_write(blank)
    good = fw.read_flash(META_A_ADDR, 48)
    corrupt = good[:8] + bytes([good[8] ^ 0x01]) + good[9:]
    fw.write_flash_raw(META_A_ADDR, corrupt)
    fw.write_flash_raw(META_B_ADDR, b"\xFF" * 48)
    check("a record with a broken CRC is not accepted", fw.metadata() is None)

    # ---------------------------------------------------------
    section("Semantic validation, not just the CRC")
    fw.reset()
    fw.lib.fw_metadata_write(blank)
    good = fw.read_flash(META_A_ADDR, 48)
    fields = list(struct.unpack(META_FMT, good))
    fields[12] = 7                                   # active_slot out of range
    body = struct.pack(META_FMT[:-1], *fields[:-1])
    forged = body + struct.pack("<I", crc32_stm32(body))
    fw.write_flash_raw(META_A_ADDR, forged)
    fw.write_flash_raw(META_B_ADDR, b"\xFF" * 48)
    check("CRC-valid record with active_slot=7 is refused", fw.metadata() is None)

    # ---------------------------------------------------------
    section("A transfer survives a reboot as IN_PROGRESS")
    fw.reset()
    img = bytes(512)
    su = struct.pack("<IIIBBH", len(img), crc32_stm32(img), 0x010000,
                     p.SLOT_B, p.PROTO_VERSION, 0)
    fw.reply(p.Frame(p.CMD_START_UPDATE, 0, su))
    fw.reply(p.Frame(p.CMD_DATA, 1, img[:256]))
    fw.reboot()                                      # flash survives, state does not
    meta = fw.metadata()
    check("interrupted transfer recorded as IN_PROGRESS",
          meta is not None and meta[7 + 3] == p.STATE_IN_PROGRESS)
    rep = fw.reply(p.Frame(p.CMD_DATA, 2, img[256:512]))
    check("DATA after reboot is refused without a new START",
          rep.cmd == p.RSP_NACK and rep.data[0] == p.ERR_STATE)

    # ---------------------------------------------------------
    print("\n" + "=" * 54)
    total = passed + failed
    print(f"{passed}/{total} tests passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
