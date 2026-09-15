# STM32 Dual-Slot OTA Bootloader

A bare-metal firmware update system for the **STM32L476RG**, written from
scratch — no HAL, no CubeMX, no RTOS.

The board receives a new firmware over UART, verifies it at three independent
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

## Features

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

No extra wiring. UART reaches the host through the on-board ST-LINK.

---

## Memory layout

```
0x08000000  ┌──────────────────┐
            │    BOOTLOADER    │   32 KB   (≈10 KB used)
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

> **Debug build** — freezes the watchdog while the core is halted at a
> breakpoint, so the board doesn't reset under the debugger:
> ```bash
> make DEBUG=1
> make flash
> ```
> Never ship a `DEBUG=1` image.

### 3 — Build the application

The application is compiled **twice**, once per slot, because each binary is
linked to a fixed address (`0x08008000` for slot A, `0x08080000` for slot B).
The same source produces both.

```bash
cd app
make            # → app_slotA.bin  app_slotB.bin
make verify     # confirms each binary is linked to the right address
```

### 4 — Flash the first application over SWD (bootstrap only)

Before the OTA path is available, bootstrap slot A directly:

```bash
cd app
make flashA     # programs app_slotA.elf into slot A via SWD
```

### 5 — Update firmware over UART

From this point forward, no SWD access is needed. A single command triggers
the update, transfers the binary, and verifies it:

```bash
python3 tools/ota_flash.py
```

The host queries the board for the free slot, sends the matching binary, and
the bootloader marks it `TESTING`. On the next boot the application must
confirm itself by writing `VALID`; if it doesn't, the watchdog resets the board
and the bootloader rolls back.

---

## OTA trigger modes

The bootloader normally listens for **2 seconds** after reset, then boots the
application. An update extends this window to **30 seconds** by writing the
magic word `0xDEADBEEF` to the last word of RAM before the reset. SRAM is not
cleared by a system reset, so the value survives.

### Default: SWD trigger (`--via-swd`)

Requires the ST-LINK to be connected (always present on the Nucleo).

```bash
python3 tools/ota_flash.py          # --via-swd is the default
```

1. Reads `_boot_request` from every built ELF and verifies all three agree on the address.
2. Opens the serial port **before** triggering, so no bootloader output is missed.
3. Uses OpenOCD to write `0xDEADBEEF` into RAM and issue a software reset.
4. Waits for `OTA request accepted` from the bootloader.
5. Hands off to `flash.py` for the transfer.

### USB-only: UART trigger (`--via-uart`)

Works with just the USB cable — no debugger attached.

```bash
python3 tools/ota_flash.py --via-uart
```

`ota_flash.py` holds the trigger byte `'U'` on the serial line. The application
requires **four consecutive polls** to see it exclusively before it sets the
boot-request flag and resets. A stray byte resets the streak, so an idle
terminal cannot accidentally trigger it.

---

## Host tools

All tools live in `tools/`. They share `protocol.py`, `transport.py`, and
`crc32.py` — the only external dependency is `pyserial`.

| Command | What it does |
|---|---|
| `python3 tools/ota_flash.py` | End-to-end update: trigger → transfer → verify |
| `python3 tools/ota_flash.py --via-uart` | Same, using UART trigger instead of SWD |
| `python3 tools/flash.py --port /dev/ttyACM0 --info` | Query board state without transferring |
| `python3 -m serial.tools.miniterm /dev/ttyACM0 115200` | Read raw UART output (`Ctrl+]` to exit) |

### Board status output

```
Board status
  protocol       : v1
  bootloader     : v0.2.0
  active firmware: v1.0.1
  active slot    : B
  free slot      : A
  state          : VALID
```

### Transfer output

```
Transfer
  size     : 4664 bytes   CRC32 : 0x1EE01898
  blocks   : 19 × 256     target: slot A
  [########################################] 100%  4664/4664 bytes
  transmitted in 0.7 s (6645 B/s)

Verification
  OK    global CRC verified
  OK    image marked TESTING
```

---

## Testing

### Host-side suites

Run entirely on the PC — no hardware needed.

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

**Autonomous rollback.** An application built to hang after two cycles was
installed. The watchdog reset the board three times, the failure counter
advanced on each attempt, and the bootloader switched back to the previous
slot — no human intervention, about twenty seconds total.

```
cycle 1 / cycle 2
*** DELIBERATE HANG — watchdog reset expected in ~3 s ***

BOOTLOADER v0.2.0  —  Reset caused by the watchdog
State : TESTING   Boot fails : 3
Failure threshold reached, rolling back → slot A
```

**Power loss mid-transfer.** Cable pulled at 51 %. The active slot stayed
`VALID`; the target slot was left `IN_PROGRESS` and correctly skipped on the
next boot.

```
Active slot : B   State : VALID
Fallback    : slot A, IN_PROGRESS, 102400 bytes
```

**Wrong binary.** Sending a slot-B image while slot A is free is rejected
before a single byte is transmitted — by the host tool and again by
`ERR_SLOT` from the bootloader.

**Corrupted frame.** A single flipped bit fails the frame CRC; the bootloader
NACKs and the host retransmits. Reprocessing is idempotent.

**Slot alternation.** Two consecutive updates without specifying a target:
the board directed the first to slot B, the second to slot A. Dumping both
slots confirmed two distinct images in flash.

---

## Design notes

### Two slots

A single-slot bootloader must erase the running firmware before writing the
new one. Any interruption — power loss, cable disconnect — leaves the device
unbootable. Two slots remove the window: the incoming image goes into the
inactive slot; the running one is never touched until the new image proves
itself.

### Two binaries per firmware version

A compiled binary is bound to its link address. An image built for
`0x08008000` will not run at `0x08080000` — function calls and interrupt
vectors are absolute. The two slots are 480 KB apart, so the build produces
two binaries from the same source. The host picks the right one after querying
the board.

```
slot A:  0080 0120  2186 0008  …   (reset handler at 0x08008621)
slot B:  0080 0120  2106 0808  …   (reset handler at 0x08080621)
         └────────┘ └────────┘
         stack ptr  same source, addresses shifted by 0x78000 (480 KB)
```

Three alternatives were considered:

| Approach | Verdict |
|---|---|
| Position-independent code (`-fPIC`) | Rejected — the vector table holds absolute addresses read directly by hardware |
| Copy to a fixed execution slot | Rejected — the copy itself has a destruction window on power loss |
| Hardware bank swap (`BFB2`) | Rejected — on STM32L4 this goes through the ST ROM bootloader, bypassing ours |
| **Two binaries** | **Chosen** — one write per byte, no scratch region, instant rollback |

### `TESTING` state

A correct CRC proves *integrity*, not *correctness*. A freshly installed image
is marked `TESTING`. The bootloader increments the failure counter **before**
the jump, then boots it. The application must write `VALID` after running
successfully. If it never does, the counter reaches its threshold and the
bootloader falls back.

### Watchdog starts in the bootloader

Rollback depends on a failing application causing a reset. Without a watchdog
a hung application simply freezes. The IWDG is started on the **first line of
`main()`** in the bootloader — so a crash in `startup.s` or a HardFault on
the first instruction is also caught. The bootloader refreshes it during update
mode; the application refreshes it conditionally, only when its work unit
actually advanced.

The three-second timeout is deliberately generous. A false positive (a healthy
firmware rolled back for no reason) is far more damaging than a slightly delayed
detection. With the LSI specified at ±5 %, the real timeout lies between 2.86 s
and 3.16 s.

### Interrupt-driven UART + ring buffer

At 115200 baud a byte arrives every 87 µs. A page erase takes ~20 ms — long
enough to lose over 200 bytes if the CPU were polling. Reception uses a
lock-free single-producer / single-consumer ring buffer. The ISR writes `head`
and reads `tail`; the main loop writes `tail` and reads `head`. No critical
section needed.

### Duplicated metadata pages

The metadata must survive power loss. Updating flash requires erasing a page
first; a loss during that erase would destroy the state needed to recover. Two
pages solve this: only the inactive one is ever erased. Between two valid
copies the higher counter wins. Each copy carries a CRC over its own contents
— a valid magic alone is insufficient because it shares its 8-byte write unit
with the counter.

### A design flaw found by testing

The original metadata described only the *active* firmware (one size, one CRC,
one version). After rollback, that structure pointed at the fallback slot while
still describing the rejected image. On the next boot the bootloader recomputed
the CRC against the wrong reference and refused to boot — rollback had saved
the device once, then bricked it on the following restart.

The fix: each slot carries its own size, CRC, and version independently (a
partition table). Rollback then reduces to a single field write — `active_slot`
— and both images keep their metadata at all times.

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
│   ├── main.c          demo application with self-confirmation logic
│   ├── startup.s       same vector table, different link address
│   ├── linker_slotA.ld 0x08008000
│   └── linker_slotB.ld 0x08080000
│
├── drivers/            shared hardware drivers, used by both images
│   ├── crc.c           hardware CRC32 peripheral
│   ├── flash.c         erase, write with read-back, bootloader-region guard
│   ├── metadata_mgr.c  dual-page persistent state manager
│   ├── iwdg.c          independent watchdog
│   └── fault.c         HardFault/MemManage/BusFault handler (reports over UART)
│
├── shared/             headers included by both images and the drivers
│
├── tools/
│   ├── ota_flash.py    end-to-end update tool (trigger + transfer + verify)
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
no protection against a deliberately crafted image. Hardening requires replacing
the whole-image CRC with an ECDSA signature checked against a write-protected
public key. The transport and protocol are unchanged; only the final validation
step differs.

**Fixed update window.** The bootloader listens for 2 seconds after reset. The
boot-request mechanism extends this to 30 seconds. No physical button required.

**Dual binaries.** The host holds two images per firmware version. Hardware with
true bank remapping could use one.

**The bootloader cannot update itself.** Fixing a bug requires SWD access or the
ST ROM bootloader via BOOT0. A two-stage design — small immutable first stage,
replaceable second stage — would lift this.

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
