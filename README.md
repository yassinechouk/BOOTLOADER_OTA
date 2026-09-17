# STM32 Dual-Slot OTA Bootloader

A bare-metal firmware update system for the **STM32L476RG**, written from
scratch — no HAL, no CubeMX, no RTOS.

The board receives a new firmware image and verifies it at three independent
levels, installs it in a spare flash slot, and boots it on trial. If the
application never confirms that it started correctly, the watchdog resets the
board and the bootloader rolls back to the previous image — with no host
involvement, no button press, and no physical access.

```
========================================
  BOOTLOADER v0.2.0
========================================
Reset caused by the watchdog
Active slot : B   State : TESTING   Boot fails : 3
Failure threshold reached, rolling back → slot A
```

---

![System Architecture](docs/arch_diagram.jpg)

---

## Features

- **Three update paths**: SWD (ST-Link), UART trigger, or **Wi-Fi via ESP32 gateway**
- **OTA firmware update** over UART using a custom binary protocol
- **Three-layer verification**: per-frame CRC32, read-back after write, whole-image CRC from flash
- **Dual application slots** — a failed update never bricks the device
- **Automatic rollback** via independent watchdog if the new image doesn't self-confirm
- **Remote trigger** — no reset button, no physical access needed
- **Power-loss safe** at every step: invalidation marker written before any destructive operation
- **Duplicated, CRC-protected metadata** in flash — state survives erase failures

---

## Hardware

| Item | Notes |
|---|---|
| NUCLEO-L476RG | Cortex-M4F · 1 MB flash (2 × 512 KB banks) · 128 KB RAM |
| Mini-USB cable | Powers the board, carries SWD and the virtual COM port |
| ESP32-S3 *(optional)* | Wi-Fi gateway — wireless OTA with no debugger or USB cable |

**ESP32 wiring** (only needed for Wi-Fi mode):

```
STM32 PA9  (USART1 TX, CN10 pin 21)  →  ESP32 IO18
STM32 PA10 (USART1 RX, CN10 pin 33)  ←  ESP32 IO17
STM32 GND  (CN10 pin 9)             ↔  ESP32 GND
```

Each board has its own USB power supply. The shared GND is mandatory — without
it the UART link works intermittently, which is harder to diagnose than not
working at all.

---

## Memory layout

```
0x08000000  ┌──────────────────┐
            │    BOOTLOADER    │   32 KB   (≈11 KB used)
0x08008000  ├──────────────────┤
            │      SLOT A      │  480 KB
0x08080000  ├──────────────────┤
            │      SLOT B      │  480 KB
0x080FF000  ├──────────────────┤
            │   METADATA A     │    2 KB
0x080FF800  ├──────────────────┤
            │   METADATA B     │    2 KB
0x08100000  └──────────────────┘
```



Each slot is independent: it has its own size, CRC32, version, and state
(`EMPTY` / `IN_PROGRESS` / `TESTING` / `VALID`). Rollback is a single field
write — `active_slot` — with no metadata recomputation.

---

## Quick start

### 1 — Install dependencies

```bash
sudo apt install gcc-arm-none-eabi gdb-multiarch openocd
pip install pyserial --break-system-packages
```

### 2 — Build and flash the bootloader

The bootloader can only be installed over SWD (once, at provisioning time).

```bash
cd bootloader
make
make flash
```

> **Debug build** — freezes the watchdog while halted at a breakpoint:
> ```bash
> make DEBUG=1 && make flash
> ```
> Never ship a `DEBUG=1` image.

### 3 — Build the application

The application is compiled **twice**, once per slot, because each binary is
linked to a fixed address (`0x08008000` for slot A, `0x08080000` for slot B).

```bash
cd app
make            # → app_slotA.bin  app_slotB.bin
make verify     # confirms each binary is linked to the right address
```

### 4 — Bootstrap slot A over SWD (first time only)

```bash
cd app
make flashA
```

### 5 — Update firmware (choose your trigger mode)

From this point forward, no SWD access is needed.

---

## OTA trigger modes

The bootloader normally listens for **2 seconds** after reset. Triggering an
update writes `0xDEADBEEF` to the last word of RAM before the reset — SRAM is
not cleared by a system reset, so the bootloader finds it on the next boot and
extends the listen window to **30 seconds**.

### Mode 1 — SWD (ST-Link connected)

```bash
python3 tools/ota_flash.py
```

Uses OpenOCD to write the magic word and issue a software reset. Reads the
`_boot_request` symbol from all three ELF files first to verify they agree on
the address.

### Mode 2 — UART only (USB cable, no debugger)

```bash
python3 tools/ota_flash.py --via-uart
```

Holds the trigger byte `'U'` on the serial line. The application requires
**4 consecutive polls** to see it exclusively before setting the flag and
resetting. A stray byte resets the streak, so an idle terminal cannot trigger
it accidentally.

### Mode 3 — Wi-Fi (ESP32 gateway, fully wireless)

Flash the ESP32 sketch once:
```
gateway/esp32_ota/esp32_ota.ino  →  Arduino IDE → ESP32-S3
```

Then, from any machine connected to the `STM32-OTA` Wi-Fi network
(password: `bootloader`):

```bash
python3 tools/ota_flash.py --via-wifi
# custom IP:
python3 tools/ota_flash.py --via-wifi 10.0.0.1
```

**What happens automatically:**

| Step | Who | What |
|---|---|---|
| 1 | Python → gateway | Opens a TCP socket to `192.168.4.1:3333` |
| 2 | Gateway | Forwards every byte to USART1, and back. It parses nothing |
| 3 | Python ↔ STM32 | `GET_INFO` answers with the free slot |
| 4 | Python | Selects `app_slotA.bin` or `app_slotB.bin` automatically |
| 5 | Python ↔ STM32 | The ordinary framed transfer, end to end over the socket |

The gateway is a transparent pipe, not a peer. The host tool talks to the
bootloader exactly as it does over a wire, so framing exists once on each end
and nowhere in between.

No file selection. No button press. No USB cable near the STM32.

**Expected output:**
```
Firmware update
  gateway 192.168.4.1:3333 over WiFi

Board status
  protocol       : v1
  bootloader     : v0.2.0
  active slot    : A
  free slot      : B
  file           : app_slotB.bin

Transfer
  size     : 6328 bytes
  CRC32    : 0xAA83AF3C
  blocks   : 25 x 256
  [########################################] 100%  6328/6328 bytes

Verification
  OK    global CRC verified
  OK    image marked TESTING
```

---

## Host tools

All tools live in `tools/`. Dependencies: `pyserial` only (Wi-Fi mode uses
Python stdlib `http.client` + `urllib`).

| Command | What it does |
|---|---|
| `python3 tools/ota_flash.py` | End-to-end update via SWD trigger |
| `python3 tools/ota_flash.py --via-uart` | Same, UART trigger |
| `python3 tools/ota_flash.py --via-wifi` | Same, Wi-Fi gateway |
| `python3 tools/flash.py --port /dev/ttyACM0 --info` | Query board state without updating |
| `python3 -m serial.tools.miniterm /dev/ttyACM0 115200` | Read raw UART output (`Ctrl+]` to exit) |

---

## Testing

### Host-side suites

```bash
cd tools
python3 test_protocol.py     # protocol state machine
python3 bootloader_sim.py    # full transfer against the simulator
python3 debug_gui.py         # step-through visualiser with fault injection
```

| Suite | Tests | Covers |
|---|---|---|
| Protocol state machine | 47 | framing, resync, retransmission, timeouts, all error codes |
| Metadata manager | 32 | dual-page selection, power loss during erase and write, counter ties |
| Ring buffer | 22 | FIFO order, wraparound, saturation accounting |
| Rollback scenario | 15 | two-image lifecycle across rollback and reinstall |

### On-silicon results

| Module | Tests | Result |
|---|---|---|
| CRC32 | 3 | vectors matching the Python implementation exactly |
| Flash driver | 10 | all five rejection paths covered |
| Metadata manager | 25 | persistence verified across bootloader reflash |
| UART | sustained burst | 115200 baud · zero bytes lost · buffer reaching 511/511 |
| IWDG | timeout + freeze | measured timeout · debug freeze verified · reset cause reported |

### Robustness scenarios (on hardware)

**Autonomous rollback.** Application built to hang after two cycles. Watchdog
reset the board three times, failure counter advanced on each, bootloader
switched to the previous slot — no human intervention, ~20 seconds total.

```
*** DELIBERATE HANG — watchdog reset expected in ~3 s ***

BOOTLOADER v0.2.0  —  Reset caused by the watchdog
State : TESTING   Boot fails : 3
Failure threshold reached, rolling back → slot A
```

**Power loss mid-transfer.** Cable pulled at 51 %. Active slot stayed `VALID`;
target slot left `IN_PROGRESS` and correctly skipped.

**Wrong binary.** Slot-B image while slot A is free — rejected before a single
byte is transmitted, by the host tool and again by `ERR_SLOT`.

**Corrupted frame.** Single flipped bit fails the CRC; bootloader NACKs, host
retransmits. Idempotent.

**Wi-Fi OTA.** Full wireless update verified end-to-end: trigger → transfer →
self-confirmation → next update directed to the alternating slot.

---

## Design notes

### Two slots

A single-slot bootloader must erase the running firmware before writing the
new one. Any interruption leaves the device unbootable. Two slots remove the
window: the incoming image goes into the inactive slot; the running one is
never touched until the new image proves itself.

### Two binaries per firmware version

A compiled binary is bound to its link address. The two slots are 480 KB apart,
so the build produces two binaries from the same source. The host picks the
right one after querying the board — or in Wi-Fi mode, does so automatically.

```
slot A:  0080 0120  2186 0008  …   (reset handler at 0x08008621)
slot B:  0080 0120  2106 0808  …   (reset handler at 0x08080621)
         └────────┘ └────────┘
         stack ptr  addresses shifted by 0x78000 (480 KB)
```

### `TESTING` state

A correct CRC proves *integrity*, not *correctness*. A freshly installed image
is marked `TESTING`. The bootloader increments the failure counter **before**
the jump. The application must write `VALID` after running successfully. If it
never does, the counter reaches threshold and the bootloader falls back.

### Watchdog starts in the bootloader

The IWDG is started on the **first line of `main()`** in the bootloader — so a
crash in `startup.s` or a HardFault on the first instruction is also caught.
The bootloader refreshes it during update mode; the application refreshes it
conditionally, only when its work unit actually advanced.

### Wi-Fi gateway design

The ESP32 is a transparent TCP-to-UART bridge. Bytes arriving on the socket go
out of USART1; bytes arriving on USART1 go back to the socket. It holds no
image buffer, computes no CRC, and knows nothing about frames.

An earlier version served a web page, buffered the whole image in 200 KB of
RAM, and reimplemented the protocol — frame building, CRC32, sequencing, the
transfer state machine. That made it a third implementation of a protocol that
already had two, with no test covering it and no way to notice when it drifted.
It also could not be driven from a command line, which is what the link is for.

As a pipe it cannot drift, because there is nothing to keep in step. TCP
guarantees delivery and ordering over the Wi-Fi hop, while the protocol's own
CRC still covers the UART hop end to end — which is the hop where corruption
actually happens.

### Interrupt-driven UART + ring buffer

At 115200 baud a byte arrives every 87 µs. A page erase takes ~20 ms — enough
to lose 200+ bytes if the CPU were polling. Reception uses a lock-free
single-producer / single-consumer ring buffer. No critical section needed.

### Duplicated metadata pages

The metadata must survive power loss. Only the inactive page is ever erased;
between two valid copies the higher counter wins. Each copy carries a CRC —
a valid magic alone is insufficient because it shares its 8-byte write unit
with the counter.

---

## Repository layout

```
├── bootloader/
│   ├── main.c          boot decision, state machine, rollback, jump
│   ├── startup.s       vector table, .data/.bss init
│   ├── linker.ld       32 KB at 0x08000000
│   └── src/
│       ├── protocol_mgr.c   frame assembly and command dispatch
│       ├── uart.c           interrupt-driven RX, ring buffer
│       └── systick.c        millisecond time base
│
├── app/
│   ├── main.c          demo application with self-confirmation and OTA trigger
│   ├── startup.s       same vector table, different link address
│   ├── linker_slotA.ld 0x08008000
│   └── linker_slotB.ld 0x08080000
│
├── drivers/            shared hardware drivers, compiled into both images
│   ├── crc.c           hardware CRC32 peripheral
│   ├── flash.c         erase, write with read-back, bootloader-region guard
│   ├── metadata_mgr.c  dual-page persistent state manager
│   ├── iwdg.c          independent watchdog
│   └── fault.c         HardFault/MemManage/BusFault handler (reports over UART)
│
├── shared/             headers included by both images and the drivers
│
├── gateway/
│   └── esp32_ota/
│       └── esp32_ota.ino   Wi-Fi gateway (headless REST API, no web UI)
│
├── tools/
│   ├── ota_flash.py    end-to-end update tool (SWD / UART / Wi-Fi)
│   ├── flash.py        transfer and board query tool
│   ├── transport.py    serial transport layer
│   ├── protocol.py     frame encode / decode
│   ├── crc32.py        CRC32 matching the STM32 peripheral output
│   ├── bootloader_sim.py   executable specification / simulator
│   ├── test_protocol.py    host-side test suite
│   └── debug_gui.py    protocol step-through visualiser with fault injection
│
└── PROTOCOL.md         full binary protocol specification
```

---

## Limitations

**Integrity, not authenticity.** CRC32 detects accidental corruption; it offers
no protection against a deliberately crafted image. Hardening requires ECDSA
signature verification against a write-protected public key.

**Fixed update window.** The bootloader listens for 2 seconds after reset. The
boot-request mechanism extends this to 30 seconds via any trigger mode.

**Dual binaries.** The host holds two images per firmware version. Hardware
with true bank remapping could use one.

**The bootloader cannot update itself.** Fixing a bug requires SWD access or
the ST ROM bootloader via BOOT0.

---

## Planned work

- FreeRTOS application layer with a supervisor task driving conditional watchdog refresh
- CAN as a second transport (the protocol layer is already transport-agnostic)
- ECDSA firmware signature verification

---

## References

- **RM0351** — STM32L4x5/L4x6 reference manual (flash, CRC, USART, IWDG, boot)
- **UM1724** — STM32 Nucleo-64 boards user manual (pinout, ST-LINK, solder bridges)
- **DS10198** — STM32L476xx datasheet (alternate function mapping, LSI accuracy)
- **MCUboot** — reference implementation studied for comparison

---

## License

MIT
