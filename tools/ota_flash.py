#!/usr/bin/env python3
"""
ota_flash.py -- trigger an OTA reset over SWD, then hand off to flash.py.

What it is for
--------------
The bootloader listens for two seconds after every reset. Hitting that
window means pressing reset and racing it. The boot request flag removes
the race: a magic word written to the last word of RAM survives a system
reset, and a bootloader that finds it stretches the window to thirty
seconds.

flash.py already sets that flag by itself, by holding the trigger byte
on the protocol link until the application hands over. This script
exists for the one case flash.py cannot cover: an application that has
crashed, hung, or is absent, and can no longer honour anything sent to
it. OpenOCD writes the word directly into RAM and resets the board, so
no cooperation from the firmware is required.

Transport
---------
Setting the flag and transferring the image are separate concerns. The
flag goes in over SWD; the transfer goes wherever flash.py is told to
send it. The protocol runs on USART1, which the ST-Link virtual COM port
does not reach, so the usual combination is SWD to trigger and WiFi to
transfer:

    python3 tools/ota_flash.py --host 192.168.4.1

--port names the DEBUG console (USART2, /dev/ttyACM0 by default). It is
only read from, to catch the banner that proves the flag was honoured.

The flag address is read from the built ELF files rather than hard-coded,
and the script refuses to run if the three images disagree about it.

Usage
-----
    python3 tools/ota_flash.py --host 192.168.4.1
    python3 tools/ota_flash.py --host 192.168.4.1 --verbose

Anything unrecognised is passed through to flash.py.
"""

import argparse
import os
import subprocess
import sys
import time

# ---------------------------------------------------------------
# Paths / constants
# ---------------------------------------------------------------
TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(TOOLS_DIR)
APP_DIR   = os.path.join(REPO_ROOT, 'app')
BL_DIR    = os.path.join(REPO_ROOT, 'bootloader')
FLASH_PY  = os.path.join(TOOLS_DIR, 'flash.py')

# Every image that has an opinion about where the flag lives.
ELF_FILES = [
    os.path.join(BL_DIR,  'bootloader.elf'),
    os.path.join(APP_DIR, 'app_slotA.elf'),
    os.path.join(APP_DIR, 'app_slotB.elf'),
]

NM = 'arm-none-eabi-nm'

# Must match shared/boot_request.h.
BOOT_REQUEST_MAGIC = 0xDEADBEEF

# Must match app/main.c.

OPENOCD_IFACE  = 'interface/stlink.cfg'
OPENOCD_TARGET = 'target/stm32l4x.cfg'

# The bootloader prints this once it has accepted the request. It is
# the only positive confirmation that the flag survived the reset and
# was understood.
BL_BANNER  = "OTA request accepted"


# ---------------------------------------------------------------

try:
    import serial
except ImportError:
    print("pyserial is required:  pip install pyserial --break-system-packages")
    sys.exit(1)

# ---------------------------------------------------------------
# Terminal colours
# ---------------------------------------------------------------
GREEN  = "\033[92m"
RED    = "\033[91m"
BOLD   = "\033[1m"
END    = "\033[0m"

def ok(msg):   print(f"  {GREEN}OK{END}    {msg}")
def err(msg):  print(f"  {RED}FAILED{END} {msg}")
def info(msg): print(f"  {msg}")
def step(msg): print(f"\n{BOLD}{msg}{END}")


# ---------------------------------------------------------------
# Where the flag lives
# ---------------------------------------------------------------

def symbol_address(elf: str, name: str):
    """Address of a symbol in an ELF, or None."""
    try:
        out = subprocess.run([NM, elf], capture_output=True, text=True,
                             timeout=15)
    except FileNotFoundError:
        err(f"{NM} not found -- is the ARM toolchain on PATH?")
        return None
    except subprocess.TimeoutExpired:
        err(f"{NM} timed out on {elf}")
        return None

    if out.returncode != 0:
        return None

    for line in out.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[2] == name:
            return int(parts[0], 16)
    return None


def resolve_boot_request_addr():
    """
    Read _boot_request from every built image and require agreement.

    Reading it rather than hard-coding it is the point: the address is
    a property of the linker scripts, and a constant duplicated here
    would keep matching right up until someone changed LENGTH(RAM), at
    which point the flag would land inside the live stack and this tool
    would still cheerfully write to the old address.
    """
    step("Boot request address")

    found = {}
    for elf in ELF_FILES:
        if not os.path.exists(elf):
            err(f"{os.path.relpath(elf, REPO_ROOT)} not built")
            info("build first:  make -C bootloader && make -C app")
            return None
        addr = symbol_address(elf, '_boot_request')
        if addr is None:
            err(f"_boot_request not found in {os.path.relpath(elf, REPO_ROOT)}")
            info("that image predates the boot request flag -- rebuild it")
            return None
        found[elf] = addr

    if len(set(found.values())) != 1:
        err("images disagree about where the flag lives:")
        for elf, addr in found.items():
            info(f"  0x{addr:08X}  {os.path.relpath(elf, REPO_ROOT)}")
        info("rebuild everything from the same linker scripts")
        return None

    addr = next(iter(found.values()))
    ok(f"0x{addr:08X} (agreed by all {len(found)} images)")
    return addr


# ---------------------------------------------------------------
# Serial helpers
# ---------------------------------------------------------------

def open_port(port: str, baud: int):
    try:
        return serial.Serial(port, baud, timeout=0.1,
                             dsrdtr=False, rtscts=False)
    except serial.SerialException as exc:
        err(f"cannot open {port}: {exc}")
        info("another program may be holding it (minicom, screen, picocom)")
        return None


def watch_for(ser, needle: str, timeout: float, echo=True, prime="") -> bool:
    """
    Read until `needle` appears in the output, or `timeout` elapses.

    `prime` is text already read from this port by an earlier stage. A
    single read() can span a board reset -- returning the line that
    ended one stage AND the banner that opens the next -- so a stage
    that stops at its own marker has to hand the remainder on. Dropping
    it loses the banner outright and the wait times out on a board that
    did everything right.
    """
    buf = prime
    if echo and prime:
        for line in prime.splitlines():
            line = line.strip()
            if line:
                info(f"board > {line}")
    if needle in buf:
        return True

    deadline = time.time() + timeout

    while time.time() < deadline:
        chunk = ser.read(256)
        if not chunk:
            continue
        text = chunk.decode("ascii", errors="replace")
        buf += text
        if echo:
            for line in text.splitlines():
                line = line.strip()
                if line:
                    info(f"board > {line}")
        if needle in buf:
            return True

    return False


# ---------------------------------------------------------------
# Trigger: SWD
# ---------------------------------------------------------------

def trigger_via_swd(addr: int, ser) -> bool:
    """
    Halt the target, write the magic word into RAM, reset and run.

    The serial port is already open before this runs. The bootloader
    prints its confirmation within milliseconds of the reset, so a
    script that opened the port afterwards would routinely miss the
    one line that proves the mechanism worked.
    """
    step("Trigger (SWD)")
    info(f"writing 0x{BOOT_REQUEST_MAGIC:08X} -> 0x{addr:08X}")

    cmd = [
        'openocd',
        '-f', OPENOCD_IFACE,
        '-f', OPENOCD_TARGET,
        '-c', (f"init; halt; "
               f"mww 0x{addr:08X} 0x{BOOT_REQUEST_MAGIC:08X}; "
               f"reset run; exit"),
    ]

    try:
        result = subprocess.run(cmd, capture_output=True, text=True,
                                timeout=15)
    except FileNotFoundError:
        err("openocd not found -- is it installed?")
        return False
    except subprocess.TimeoutExpired:
        err("openocd timed out")
        return False

    if result.returncode != 0:
        err("openocd failed")
        for line in result.stderr.splitlines():
            if any(k in line for k in ('Error', 'failed', 'FAILED')):
                info(f"  {line}")
        return False

    ok("magic word written, target reset")
    return True


# ---------------------------------------------------------------
# Trigger: UART
# ---------------------------------------------------------------

def run_flash(port: str, extra_args: list) -> int:
    """
    Hand the transfer to flash.py.

    --port is passed for the wired case, but flash.py ignores it when
    --host is present, so the transport is whatever the caller asked
    for. That matters here: the protocol runs on USART1, which the
    ST-Link port does not reach. Triggering over SWD and transferring
    over WiFi is the normal combination:

        ota_flash.py --via-swd --host 192.168.4.1
    """
    step("Firmware transfer")
    cmd = [sys.executable, FLASH_PY,
           '--port', port,
           '--dir', APP_DIR] + extra_args
    return subprocess.call(cmd)


# ---------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(
        description="OTA update without pressing reset",
        epilog="unrecognised arguments are passed through to flash.py")
    ap.add_argument('--port', default='/dev/ttyACM0')
    ap.add_argument('--baud', type=int, default=115200)
    ap.add_argument('--via-swd', action='store_true',
                    help="trigger with OpenOCD over SWD (the only mode)")

    args, extra = ap.parse_known_args()


    # Wi-Fi path is fully self-contained — no serial port, no ELF checks.

    addr = resolve_boot_request_addr()
    if addr is None:
        return 1

    step("Serial port")
    ser = open_port(args.port, args.baud)
    if ser is None:
        return 1
    ok(f"{args.port} @ {args.baud} baud")

    try:
        if not trigger_via_swd(addr, ser):
            err("could not set the boot request flag")
            info("is the ST-Link connected and the board powered?")
            return 1

        # The one check that proves the whole mechanism worked. Every
        # earlier version of this script shrugged and carried on here,
        # which meant a bootloader that ignored the flag was
        # indistinguishable from a slow one -- flash.py just timed out
        # a few seconds later with nothing to say about why.
        step("Waiting for the bootloader")
        if not watch_for(ser, BL_BANNER, timeout=5.0):
            err("the bootloader did not report accepting the request")
            info("the flag was written and the board reset, but the banner")
            info("never arrived on the debug console. Two common causes:")
            info("  - OpenOCD disturbed the ST-Link's virtual COM port;")
            info("    unplug and replug the USB cable, then retry")
            info("  - the bootloader predates the boot request flag;")
            info("    reflash it:  make -C bootloader flash")
            info("the transfer may still work -- try flash.py directly")
            return 1
        ok("bootloader is listening")

    finally:
        ser.close()

    # Give the OS a moment to release the port before flash.py claims it.
    time.sleep(0.2)

    return run_flash(args.port, extra)


if __name__ == "__main__":
    sys.exit(main())
