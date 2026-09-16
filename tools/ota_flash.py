#!/usr/bin/env python3
"""
ota_flash.py -- one-command OTA update, without the reset button.

What it does
------------
The bootloader listens on the serial link for two seconds after every
reset. That window is deliberately short, and hitting it means
resetting the board and racing it. The boot request flag removes the
race: a magic word written to the last word of RAM survives a system
reset, and a bootloader that finds it stretches the same listen window
to thirty seconds instead.

This script sets that flag, resets the board, waits for the bootloader
to confirm, and hands over to flash.py.

Three trigger modes
-------------------
  --via-swd   (default)  OpenOCD writes the word straight into RAM and
                         resets. Needs an ST-Link.

  --via-uart             Holds the trigger character down on the serial
                         line until the running application confirms it
                         and resets itself.

  --via-wifi [IP]        Uses the ESP32 Wi-Fi gateway (default IP:
                         192.168.4.1). Fully automated — the gateway
                         triggers the OTA reset, this script queries
                         which slot is free, uploads the correct .bin,
                         starts the flash, and streams the log.
                         Connect your PC to the "STM32-OTA" Wi-Fi
                         network before running.

The flag address (for SWD/UART modes) is read from the built ELF
files so that a constant duplicated here can never silently drift.

Usage
-----
    python3 tools/ota_flash.py                     # SWD trigger (default)
    python3 tools/ota_flash.py --via-uart
    python3 tools/ota_flash.py --via-wifi           # 192.168.4.1
    python3 tools/ota_flash.py --via-wifi 10.0.0.1
    python3 tools/ota_flash.py --port /dev/ttyUSB0 --verbose

Anything unrecognised is passed through to flash.py (SWD/UART modes).
"""

import argparse
import http.client
import mimetypes
import os
import subprocess
import sys
import time
import urllib.request
import urllib.error

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
OTA_TRIGGER_BYTE = b'U'

OPENOCD_IFACE  = 'interface/stlink.cfg'
OPENOCD_TARGET = 'target/stm32l4x.cfg'

# The bootloader prints this once it has accepted the request. It is
# the only positive confirmation that the flag survived the reset and
# was understood.
BL_BANNER  = "OTA request accepted"

# The application prints this when the trigger streak completes.
APP_BANNER = "OTA request confirmed"

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

def trigger_via_uart(ser, timeout: float = 10.0):
    """
    Hold the trigger character down until the application confirms.

    The application requires several consecutive polls to see the
    trigger character and nothing else before it acts -- see the OTA
    trigger section in app/main.c. It polls roughly once per blink, so
    confirmation takes a couple of seconds of sustained sending. That
    is the cost of not having a single byte able to reboot the board.

    Sending stops the moment the application confirms, so that none of
    these bytes are still in flight when flash.py starts talking to the
    bootloader.

    Returns whatever was read past the application's confirmation (the
    caller needs it -- see watch_for), or None if the request was never
    confirmed.
    """
    step("Trigger (UART)")
    info(f"holding {OTA_TRIGGER_BYTE!r} down until the application confirms")

    ser.reset_input_buffer()

    buf = ""
    deadline = time.time() + timeout

    while time.time() < deadline:
        ser.write(OTA_TRIGGER_BYTE * 8)
        ser.flush()

        chunk = ser.read(256)
        if chunk:
            buf += chunk.decode("ascii", errors="replace")
            if APP_BANNER in buf:
                ok("application confirmed the request")
                # Hand the tail on: this read may already contain the
                # bootloader's banner, printed milliseconds later.
                return buf.split(APP_BANNER, 1)[1]

        time.sleep(0.02)

    err("the application never confirmed the request")
    info("is it running its main loop? is the serial port the right one?")
    info("if the application is crashed or absent, use --via-swd instead")
    return None


# ---------------------------------------------------------------
# Wi-Fi flash (ESP32 gateway)
# ---------------------------------------------------------------

ESP32_DEFAULT_IP  = "192.168.4.1"
ESP32_TIMEOUT_S   = 30          # /prepare blocks for ~1 s, /flash streams
LOG_POLL_INTERVAL = 0.5


def _http_get(ip: str, path: str, timeout: int = ESP32_TIMEOUT_S) -> str:
    """Plain GET, returns body text or raises with the response body included."""
    url = f"http://{ip}{path}"
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return r.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", errors="replace").strip()
        raise RuntimeError(f"HTTP {exc.code} from {path}: {body}") from None



def _http_post_binary(ip: str, path: str, data: bytes,
                      filename: str) -> str:
    """
    Multipart POST of a binary file — the same form field ('f') that
    the ESP32's handleUploadData() expects.
    """
    boundary = b"----OTABoundary7a4d"
    body  = b"--" + boundary + b"\r\n"
    body += (f'Content-Disposition: form-data; name="f"; '
             f'filename="{filename}"\r\n').encode()
    body += b"Content-Type: application/octet-stream\r\n\r\n"
    body += data
    body += b"\r\n--" + boundary + b"--\r\n"

    conn = http.client.HTTPConnection(ip, timeout=ESP32_TIMEOUT_S)
    conn.request(
        "POST", path, body,
        {"Content-Type": f"multipart/form-data; boundary={boundary.decode()}",
         "Content-Length": str(len(body))})
    resp = conn.getresponse()
    text = resp.read().decode("utf-8", errors="replace")
    conn.close()
    if resp.status != 200:
        raise RuntimeError(f"upload returned HTTP {resp.status}: {text}")
    return text


def wifi_flash(ip: str) -> int:
    """
    Full OTA flow over the ESP32 Wi-Fi gateway:
      1. GET /prepare  -> triggers OTA reset + GET_INFO, returns free slot
      2. select the matching .bin from the app build directory
      3. POST /upload  -> binary lands in ESP32 RAM
      4. GET /flash    -> ESP32 runs the protocol transfer
      5. poll GET /log -> stream progress to the terminal
    """
    step("Wi-Fi OTA  (gateway " + ip + ")")

    # --- 1. prepare (triggers OTA reset + GET_INFO on the STM32) ---
    info("calling /prepare  (OTA reset + GET_INFO)...")
    try:
        slot_char = _http_get(ip, "/prepare", timeout=15).strip()   # 'A' or 'B'
    except Exception as exc:
        err(f"/prepare failed: {exc}")
        info("is the PC connected to the STM32-OTA Wi-Fi network?")
        info("is the ESP32 powered and the STM32 running its main loop?")
        return 1

    if slot_char not in ("A", "B"):
        err(f"unexpected /prepare response: {slot_char!r}")
        return 1
    ok(f"free slot is {slot_char}")

    # --- 2. select the binary ---
    bin_name = f"app_slot{slot_char}.bin"
    bin_path = os.path.join(APP_DIR, bin_name)
    if not os.path.exists(bin_path):
        err(f"{bin_path} not found")
        info("build the application first:  make -C app")
        return 1

    with open(bin_path, "rb") as f:
        firmware = f.read()
    ok(f"selected {bin_name}  ({len(firmware)} bytes)")

    # --- 3. upload ---
    step("Uploading firmware to ESP32 RAM")
    try:
        resp = _http_post_binary(ip, "/upload", firmware, bin_name)
    except Exception as exc:
        err(f"/upload failed: {exc}")
        return 1
    ok(resp.strip())

    # --- 4. trigger the flash ---
    step("Firmware transfer")
    try:
        _http_get(ip, "/flash", timeout=5)
    except Exception as exc:
        err(f"/flash failed: {exc}")
        return 1

    # --- 5. stream logs ---
    seen_lines = 0
    deadline   = time.time() + 60          # 60 s max; transfer is ~1 s
    success    = False

    while time.time() < deadline:
        time.sleep(LOG_POLL_INTERVAL)
        try:
            log_text = _http_get(ip, "/log", timeout=5)
        except Exception:
            continue

        lines = [l for l in log_text.splitlines() if l.strip()]
        for line in lines[seen_lines:]:
            info(f"board > {line}")
            if "transfer succeeded" in line:
                success = True
            if "transfer failed" in line:
                err("transfer failed (see board log above)")
                return 1
        seen_lines = len(lines)
        if success:
            break

    if not success:
        err("timed out waiting for transfer to complete")
        return 1

    ok("done")
    return 0


# ---------------------------------------------------------------
# Serial flash
# ---------------------------------------------------------------

def run_flash(port: str, extra_args: list) -> int:
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
    ap.add_argument('--via-uart', action='store_true',
                    help="trigger over the serial link instead of SWD")
    ap.add_argument('--via-swd', action='store_true',
                    help="trigger with OpenOCD over SWD (default)")
    ap.add_argument('--via-wifi', nargs='?', const=ESP32_DEFAULT_IP,
                    metavar='IP',
                    help="use the ESP32 Wi-Fi gateway "
                         f"(default IP: {ESP32_DEFAULT_IP})")

    args, extra = ap.parse_known_args()

    modes = sum([bool(args.via_uart), bool(args.via_swd),
                 args.via_wifi is not None])
    if modes > 1:
        ap.error("--via-uart, --via-swd and --via-wifi are mutually exclusive")

    # Wi-Fi path is fully self-contained — no serial port, no ELF checks.
    if args.via_wifi is not None:
        return wifi_flash(args.via_wifi)

    addr = resolve_boot_request_addr()
    if addr is None:
        return 1

    step("Serial port")
    ser = open_port(args.port, args.baud)
    if ser is None:
        return 1
    ok(f"{args.port} @ {args.baud} baud")

    prime = ""

    try:
        if args.via_uart:
            prime = trigger_via_uart(ser)
            if prime is None:
                return 1
        else:
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
        if not watch_for(ser, BL_BANNER, timeout=5.0, prime=prime):
            err("the bootloader did not accept the request")
            info("it reset, but never reported the flag. The usual cause is")
            info("a bootloader older than v0.2.0: its stack reaches the top")
            info("of RAM, so its own first push overwrites the flag before")
            info("it is read. Reflash it over SWD:  make -C bootloader flash")
            return 1
        ok("bootloader is listening")

    finally:
        ser.close()

    # Give the OS a moment to release the port before flash.py claims it.
    time.sleep(0.2)

    return run_flash(args.port, extra)


if __name__ == "__main__":
    sys.exit(main())
