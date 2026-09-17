# STM32 Dual-Slot OTA Bootloader

A bare-metal firmware update system for the **STM32L476RG**, written from
scratch — no HAL, no CubeMX, no RTOS.

The board receives a new firmware image, verifies it at three independent
levels, installs it in a spare flash slot, and boots it on trial. If the
application never confirms that it started correctly, the watchdog resets the
board and the bootloader rolls back to the previous image — with no host
involvement and no button press.

```
========================================
  BOOTLOADER v0.2.0
========================================
Active slot : B
State       : TESTING
Boot fails  : 3
Fallback    : slot A, VALID, 6328 bytes

Failure threshold reached, rolling back
Falling back to slot A
```

---

![System Architecture](docs/arch_diagram.jpg)

---

## Features

- **Dual application slots** — a failed update never bricks the device
- **Three-layer verification** — per-frame CRC32, read-back after every write, whole-image CRC re-read from flash
- **Automatic rollback** — the independent watchdog resets a firmware that never self-confirms; after three attempts the bootloader switches back
- **Power-loss safe** — the invalidation marker is written before any destructive operation, and metadata is duplicated across two flash pages
- **Wireless OTA** — an ESP32 acts as a transparent TCP-to-UART bridge; one command flashes the board over Wi-Fi
- **No reset button** — a magic word in the last RAM word survives a software reset and extends the listen window
- **Fault reporting** — HardFault/MemManage/BusFault/UsageFault dump PC, LR and the fault status registers over UART
- **Host-tested C** — the firmware's own state machines are compiled for the PC and driven through 43 tests

---

## Two serial links — read this first

The board carries **two independent UARTs**, and confusing them is the single
most common way to waste an hour:

| Link | Pins | Connected to | Direction | Carries |
|---|---|---|---|---|
| `uart_debug` — USART2 | PA2 / PA3 | ST-Link VCP (`/dev/ttyACM0`) | **transmit only** | human-readable logs |
| `uart_proto` — USART1 | PA9 / PA10 | ESP32 gateway | full duplex | the update protocol |

`uart_debug_init()` never sets `RE`, so **the board cannot receive anything on
`/dev/ttyACM0`**. That port is for reading logs, nothing else.

Consequently there are exactly three ways to get firmware onto the board:

| Path | Needs | Command |
|---|---|---|
| **Wi-Fi** | ESP32 gateway | `flash.py --host 192.168.4.1` |
| **Wired protocol** | USB-TTL adapter on PA9/PA10 | `flash.py --port /dev/ttyUSB0` |
| **SWD** | ST-Link (built into the Nucleo) | `make flash` + `write_metadata.py` |

---

## Hardware

| Item | Notes |
|---|---|
| NUCLEO-L476RG | Cortex-M4F · 1 MB flash (2 × 512 KB banks) · 128 KB RAM |
| Mini-USB cable | Powers the board, carries SWD and the virtual COM port |
| ESP32 *(optional)* | Wi-Fi gateway — required for wireless OTA |

**ESP32 wiring:**

```
STM32 PA9  (USART1 TX, CN10 pin 21)  →  ESP32 GPIO18
STM32 PA10 (USART1 RX, CN10 pin 33)  ←  ESP32 GPIO17
STM32 GND  (CN10 pin 9)              ↔  ESP32 GND
```

Each board has its own USB power supply. The shared GND is mandatory — without
it the link works intermittently, which is harder to diagnose than not working
at all.

---

## Memory layout

```
0x08000000  ┌──────────────────┐
            │    BOOTLOADER    │   32 KB   (11204 bytes used, 34 %)
0x08008000  ├──────────────────┤
            │      SLOT A      │  480 KB   (bank 1)
0x08080000  ├──────────────────┤
            │      SLOT B      │  480 KB   (bank 2)
0x080FF000  ├──────────────────┤
            │   METADATA A     │    2 KB
0x080FF800  ├──────────────────┤
            │   METADATA B     │    2 KB
0x08100000  └──────────────────┘
```

Each slot carries its **own** size, CRC32, version and state (`EMPTY` /
`IN_PROGRESS` / `TESTING` / `VALID`). Rollback is a single field write —
`active_slot` — with no metadata recomputation.

The boot-request flag lives at `0x20017FFC`, the last word of RAM, with
`_estack` lowered to `0x20017FF8` so the stack cannot reach it. Both addresses
come from the linker scripts, never from a constant in C.

---

## Quick start

### 1 — Dependencies

```bash
sudo apt install gcc-arm-none-eabi openocd picocom
```

```bash
pip install pyserial --break-system-packages
```

### 2 — Build everything

```bash
cd /path/to/bootloader && make -C bootloader && make -C app && make -C app verify
```

`make verify` confirms each binary is linked to its own slot address. The
application is compiled **twice** because a binary is bound to its link
address — `0x08008000` for slot A, `0x08080000` for slot B.

### 3 — Install the bootloader over SWD

> **Close every serial terminal first.** OpenOCD and the ST-Link virtual COM
> port are the same USB device. A `picocom` left open causes
> `OpenOCD init failed`, which looks like a hardware fault and is not one.

```bash
pkill picocom; make -C bootloader flash
```

### 4 — Bootstrap both slots over SWD (first time only)

Flashing an image over SWD is **not enough on its own**. The bootloader refuses
to jump unless the image CRC matches the record in metadata, and SWD writes the
slot but not that record — only a completed OTA writes both. `write_metadata.py`
closes the gap.

```bash
make -C app flashA && make -C app flashB
```

```bash
python3 tools/write_metadata.py --slot B --image app/app_slotB.bin --version 1.0.0
```

```bash
python3 tools/write_metadata.py --slot A --image app/app_slotA.bin --version 1.0.0
```

Order matters: the second command must print `preserving slot B: …`, proving it
read the existing record instead of overwriting it. Describing only one slot
would leave the other `EMPTY` and silently remove the fallback image.

### 5 — Watch it boot

```bash
picocom -b 115200 /dev/ttyACM0
```

Press reset. Quit with `Ctrl-A` `Ctrl-X`.

```
========================================
  BOOTLOADER v0.2.0
========================================
Active slot : A
State       : VALID
Size        : 6328 bytes
Fallback    : slot B, VALID, 6328 bytes

Update mode (2000 ms)
Jumping to 0x08008000

########################################
  APPLICATION -- slot A
########################################
Image verification:
  computed CRC : 0xA99E2320
  expected CRC : 0xA99E2320   match

cycle 1
cycle 2
cycle 3

Confirmation:
  start-up confirmed: state VALID, counter cleared
```

---

## Updating over Wi-Fi

Flash the gateway sketch once (`gateway/esp32_ota/esp32_ota.ino`, Arduino IDE),
join the `STM32-OTA` network (password `bootloader`), then:

```bash
python3 tools/flash.py --host 192.168.4.1 --dir app/
```

That is the whole interface. No web page, no file picker, no reset button.

**What happens:** the tool tries `GET_INFO` first. If the bootloader's 2-second
window has closed and the application is running, it holds the trigger byte on
the protocol UART until the application hands over, then retries — so one
command works whether the board sits in the bootloader or is running firmware.

```
Firmware update
  gateway 192.168.4.1:3333 over WiFi

Board status

OTA request
  holding b'U' down for 4 s
  waiting for the bootloader to come up...
  OK    bootloader is listening

Board status
  protocol       : v1
  bootloader     : v0.2.0
  active firmware: v1.0.0
  active slot    : A
  free slot      : B
  state          : VALID
  file           : app_slotB.bin

Transfer
  size     : 6328 bytes
  CRC32    : 0xAA83AF3C
  blocks   : 25 x 256
  target   : slot B
  [########################################] 100%  6328/6328 bytes
  transmitted in 1.0 s (6230 B/s)

Verification
  re-reading flash and computing global CRC...
  OK    global CRC verified
  OK    image marked TESTING
  board rebooting; application must confirm itself
```

`picocom` and `flash.py --host` do **not** conflict — one holds the debug UART,
the other goes over Wi-Fi to the protocol UART. Keep the console open in one
terminal and flash from another to watch the whole lifecycle live.

Query the board without updating anything:

```bash
python3 tools/flash.py --host 192.168.4.1 --info
```

---

## Host tools

Everything lives in `tools/`. The only third-party dependency is `pyserial`,
and only for the wired paths.

| Command | What it does |
|---|---|
| `flash.py --host 192.168.4.1 --dir app/` | Update over Wi-Fi, auto-triggering if needed |
| `flash.py --host 192.168.4.1 --info` | Query board state, change nothing |
| `flash.py --port /dev/ttyUSB0 --dir app/` | Same over a USB-TTL adapter on PA9/PA10 |
| `write_metadata.py --slot A --image app/app_slotA.bin` | Write the metadata record for an SWD-flashed image |
| `write_metadata.py --slot A --image … --dry-run` | Build and print the record, touch nothing |
| `ota_flash.py --host 192.168.4.1` | Trigger the reset over SWD, then transfer over Wi-Fi |
| `crc32.py` | Print the CRC32 of the reference vectors |
| `picocom -b 115200 /dev/ttyACM0` | Read the debug console |

`ota_flash.py` exists for the one case `flash.py` cannot cover: an application
that has crashed, hung, or is absent, and can no longer honour a trigger sent to
it. OpenOCD writes the flag straight into RAM, so no cooperation from the
firmware is needed.

Setting the flag and transferring the image are separate concerns there. The
flag goes in over SWD; `--host` (or `--port`) says where the transfer should go
and is passed through to `flash.py`. It reads `_boot_request` from all three ELF
files and refuses to run if they disagree on the address.

---

## Testing

### Host suite — the real firmware C

```bash
make -C tests/host test
```

`tests/host` compiles `drivers/metadata_mgr.c` and
`bootloader/src/protocol_mgr.c` **unmodified** into a shared library and drives
them the way the board does: bytes in, bytes out. A failure there is a firmware
failure, not a model failure.

The firmware dereferences flash addresses directly, so the harness `mmap`s 1 MB
at `FLASH_BASE_ADDR` rather than redirecting through an accessor — editing the
code under test to make it testable would defeat the purpose. Struct sizes are
asserted at runtime (`metadata_t` 48 bytes, `slot_info_t` 16, `start_update_t`
16, `info_response_t` 12) so a host/target layout divergence fails loudly.

**43 tests** covering: protocol state machine, sequencing, retransmission
idempotence, checks-before-erase ordering, metadata A/B selection and CRC,
semantic validation, and interrupted transfers.

Only the register-level drivers are replaced, by models of the contracts their
headers document — the flash model rejects misaligned writes and writes to
non-erased memory, because erase-before-write is a rule callers depend on. The
CRC model is checked against the vectors `shared/crc.h` records as measured on
silicon (`"123456789"` → `0x9B63D02C`).

The harness was verified to have teeth rather than assumed: removing the
`active_slot` bounds check in `metadata_mgr.c` fails exactly one test, and
removing the sequence check in `protocol_mgr.c` fails exactly one other.

### Verified on hardware

Each of these was observed on a NUCLEO-L476RG and is reproducible with the
commands above:

| Scenario | Evidence |
|---|---|
| Boot chain | `computed CRC` equals the value `write_metadata.py` computed on the host |
| OTA over Wi-Fi | full transfer, `global CRC verified`, slot alternating A → B → A |
| Trial → confirmation | `State : TESTING` → `start-up confirmed: state VALID, counter cleared` |
| Autonomous rollback | application built to never confirm; `Boot fails` 1 → 2 → 3, then `Failure threshold reached, rolling back`, fallback image boots with a matching CRC |
| Fault reporting | deliberate undefined instruction → `HARD FAULT`, `CFSR 0x00010000`, PC resolved to the exact source line with `addr2line` |
| Boot-request flag | survives a software reset; bootloader reports `OTA request accepted, listening for 30 s` |
| Watchdog | application runs hundreds of cycles without a spurious reset |

---

## Design notes

### Two slots

A single-slot bootloader must erase the running firmware before writing the new
one. Any interruption leaves the device unbootable. Two slots remove the window:
the incoming image goes into the inactive slot; the running one is never touched
until the new image proves itself.

### Two binaries per firmware version

A compiled binary is bound to its link address, and the slots are 480 KB apart,
so the build produces two binaries from the same source. The host queries the
board and picks the right one. `make verify` guards against the mistake that
would otherwise surface only as a silent crash.

### `TESTING` state

A correct CRC proves *integrity*, not *correctness* — a firmware transmitted
without a single corrupted bit can still crash on its first instruction. A
freshly installed image is marked `TESTING`. The bootloader increments the
failure counter **before** jumping, because the jump never returns. The
application must write `VALID` once it considers itself healthy; if it never
does, the counter reaches the threshold and the bootloader falls back.

### Per-slot metadata

An earlier version described only the active firmware. The flaw appeared only at
rollback: switching slots kept the size and CRC of the *rejected* image, so the
bootloader read `VALID`, checked the fallback against the wrong reference, and
refused to boot. Rollback saved the board once, then immobilised it on the next
restart. Describing both slots independently removes the problem.

### Watchdog starts in the bootloader

The IWDG is started on the first line of `main()` — before any other
initialisation — so a crash in `startup.s` or a HardFault on the first
instruction is caught too. Surveillance must begin before the thing it watches.
The application then inherits a watchdog it cannot stop, and must refresh it or
be reset.

### Wi-Fi gateway design

The ESP32 is a transparent TCP-to-UART bridge: bytes arriving on the socket go
out of USART1, bytes arriving on USART1 go back to the socket. It holds no image
buffer, computes no CRC, and knows nothing about frames.

An earlier version served a web page, buffered the whole image in 200 KB of RAM,
and reimplemented the protocol. That made it a third implementation of something
that already had two, with no test covering it. As a pipe it cannot drift,
because there is nothing to keep in step. TCP guarantees delivery and ordering
over the Wi-Fi hop; the protocol's own CRC still covers the UART hop, which is
where corruption actually happens.

### Interrupt-driven UART + ring buffer

At 115200 baud a byte arrives every 87 µs. A page erase takes roughly 20 ms —
enough to lose two hundred bytes if the CPU were polling. Reception uses a
lock-free single-producer/single-consumer ring buffer: each index has exactly
one writer, so no critical section is needed.

### Duplicated metadata pages

Only the inactive page is ever erased, so a power cut can never destroy both
copies. Between two valid copies the higher counter wins. Each carries a CRC — a
valid magic alone is insufficient, because it shares its 8-byte write unit with
the counter, and a cut after the first write would leave a valid magic in front
of fields still at `0xFF`.

---

## Repository layout

```
├── bootloader/
│   ├── main.c              boot decision, rollback, jump to application
│   ├── startup.s           vector table, FPU enable, .data/.bss init
│   ├── linker.ld           32 KB at 0x08000000
│   └── src/
│       ├── protocol_mgr.c  frame assembly and command dispatch
│       ├── uart.c          two USART instances, interrupt-driven RX
│       └── systick.c       millisecond time base
│
├── app/
│   ├── main.c              demo application: self-confirmation, OTA trigger
│   ├── linker_slotA.ld     0x08008000
│   └── linker_slotB.ld     0x08080000
│
├── drivers/                compiled into both images
│   ├── crc.c               hardware CRC32 peripheral
│   ├── flash.c             erase, write with read-back, bootloader-region guard
│   ├── metadata_mgr.c      dual-page persistent state
│   ├── iwdg.c              independent watchdog
│   └── fault.c             fault handlers, register dump over UART
│
├── shared/                 headers shared by both images
│   ├── board.h             system clock, BRR derivation, compile-time checks
│   ├── boot_request.h      the RAM flag that survives a reset
│   └── …
│
├── gateway/esp32_ota/      transparent TCP-to-UART bridge (no web UI)
│
├── tests/host/             the firmware C, compiled and tested on the PC
│   ├── fake_hw.c           models of flash, CRC and UART
│   ├── shim.c              test-facing API
│   └── test_c_firmware.py  43 tests
│
├── tools/
│   ├── flash.py            transfer and query, serial or TCP
│   ├── write_metadata.py   bootstrap metadata over SWD
│   ├── ota_flash.py        OTA trigger via OpenOCD
│   ├── transport.py        framing shared by both links
│   ├── protocol.py         frame encode / decode
│   └── crc32.py            CRC32 matching the STM32 peripheral
│
└── PROTOCOL.md             binary protocol specification
```

---

## Limitations

**Integrity, not authenticity.** CRC32 detects accidental corruption; it offers
no protection against a deliberately crafted image. Hardening requires signature
verification against a write-protected public key.

**No authentication on the gateway.** The WPA2 passphrase on the access point is
the only barrier: anyone who joins the network can flash the board. Acceptable
for a bench tool on an isolated AP, not for a product.

**No write protection on the bootloader region.** `flash.c` refuses writes that
overlap it, but that is a software interlock, not a hardware boundary. The
hardware answer is the WRP option bytes, deliberately not programmed from
firmware — a wrong WRP value cannot be undone over the serial link.

**The bootloader cannot update itself.** Fixing a bug in it requires SWD or the
ST ROM bootloader via BOOT0.

**Slot A shares bank 1 with the bootloader.** On STM32L4, programming a bank
stalls instruction fetch from that bank, so the receive ISR cannot run during a
slot-A erase. Slot B is in bank 2 and unaffected. The request/response protocol
means nothing is lost in practice, but the asymmetry is real and unmeasured.

---

## Planned work

- CAN as a second transport — the protocol layer is already transport-agnostic
- Signature verification
- An RTOS application layer with a supervisor task driving the conditional watchdog refresh

---

## References

- **RM0351** — STM32L4x5/L4x6 reference manual (flash, CRC, USART, IWDG, boot)
- **UM1724** — STM32 Nucleo-64 boards user manual (pinout, ST-Link, solder bridges)
- **DS10198** — STM32L476xx datasheet (alternate function mapping, LSI accuracy)

---

## License

MIT
