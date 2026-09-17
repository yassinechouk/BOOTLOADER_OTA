"""
Transport for the update protocol.

Two links, one framing implementation
-------------------------------------
The protocol is carried either over a serial port or over TCP to the
WiFi gateway. Only the byte-level I/O differs: framing, resynchronising
on the magic, the two-pass read and the retransmission policy are
identical, and they live once in _FrameTransport.

Writing them twice would have been the obvious shortcut and the wrong
one. This protocol already has three implementations -- the bootloader,
the simulator and the host -- and a fourth differing only in how bytes
reach the wire is how two links start behaving differently for reasons
nobody can reproduce.

Requires pyserial for the serial link:
    pip install pyserial --break-system-packages
"""

import socket
import time

import protocol as p

try:
    import serial
except ImportError:
    serial = None


class TransportError(Exception):
    """Generic transport failure."""


class Timeout(TransportError):
    """Nothing arrived within the allotted time."""


class Disconnected(TransportError):
    """The link went away: cable pulled, board reset, socket closed."""


class _FrameTransport:
    """
    Sends a frame and waits for the response, then decodes it.

    Reading is done in two passes: first the 7-byte header, from which
    the payload length is derived, then the rest. Same reasoning as on
    the firmware side -- it is impossible to know how many bytes to
    wait for before having read LENGTH.

    Subclasses supply only _write, _read, _flush_input and close.
    """

    name = "xfer"

    def __init__(self, timeout: float = 1.0, verbose: bool = False):
        self.timeout = timeout
        self.verbose = verbose

    # --- byte-level, provided by subclasses ----------------------
    def _write(self, data: bytes):
        raise NotImplementedError

    def _read(self, n: int) -> bytes:
        raise NotImplementedError

    def _flush_input(self):
        pass

    def close(self):
        raise NotImplementedError

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()

    # --- framing, shared -----------------------------------------
    def _log(self, msg: str):
        if self.verbose:
            print(f"    [{self.name}] {msg}")

    def send(self, frame: p.Frame):
        raw = p.encode(frame)
        self._log(f"-> {frame}  ({len(raw)} bytes)")
        self._write(raw)

    def receive(self, timeout: float = None) -> p.Frame:
        """
        Reads a complete frame. Raises Timeout if nothing arrives.

        Resynchronises on the magic, so spurious bytes or a truncated
        response do not block indefinitely.
        """
        deadline = time.time() + (timeout if timeout is not None else self.timeout)

        window = b""
        while time.time() < deadline:
            byte = self._read(1)
            if not byte:
                continue
            window = (window + byte)[-2:]
            if window == p.MAGIC:
                break
        else:
            raise Timeout("no preamble received")

        rest = self._read_exact(p.FRAME_HEADER_SIZE - 2, deadline)
        header = p.MAGIC + rest

        length = int.from_bytes(header[3:5], "little")
        if length > p.MAX_PAYLOAD_SIZE:
            raise TransportError(f"abnormal LENGTH: {length}")

        tail = self._read_exact(length + p.FRAME_CRC_SIZE, deadline)

        frame = p.decode(header + tail)
        self._log(f"<- {frame}")
        return frame

    def _read_exact(self, n: int, deadline: float) -> bytes:
        buf = b""
        while len(buf) < n:
            if time.time() > deadline:
                raise Timeout(f"{len(buf)}/{n} bytes received")
            chunk = self._read(n - len(buf))
            if chunk:
                buf += chunk
        return buf

    def exchange(self, frame: p.Frame, retries: int = 3) -> p.Frame:
        """
        Sends and waits for the response, retransmitting if necessary.

        A retransmission is harmless on the bootloader side: receiving
        the same frame twice produces the same result as receiving it
        once. That idempotence is what makes this loop safe.
        """
        last_error = None

        for attempt in range(retries):
            try:
                self.send(frame)
                return self.receive()

            except (Timeout, p.BadCRC, p.BadMagic) as e:
                # Recoverable: lost frame, corrupted frame, or
                # desynchronisation. Disconnected is deliberately not
                # caught -- retrying a link that has gone away only
                # delays the diagnosis.
                last_error = e
                if attempt < retries - 1:
                    self._log(f"failure ({e}), retrying")
                    self._flush_input()
                    time.sleep(0.05)

        raise TransportError(
            f"no response after {retries} attempts: {last_error}"
        )


class SerialTransport(_FrameTransport):
    """Direct serial link: ST-LINK VCP, or a USB-TTL adapter on USART1."""

    name = "ser"

    def __init__(self, port: str, baudrate: int = 115200,
                 timeout: float = 1.0, verbose: bool = False):
        super().__init__(timeout, verbose)
        if serial is None:
            raise TransportError(
                "pyserial is required: pip install pyserial --break-system-packages")
        try:
            self.ser = serial.Serial(port, baudrate, timeout=timeout)
        except serial.SerialException as e:
            raise TransportError(f"cannot open {port}: {e}")

        # The ST-LINK may hold bytes from a previous session. Start clean.
        time.sleep(0.05)
        self.ser.reset_input_buffer()
        self.ser.reset_output_buffer()

    def _write(self, data: bytes):
        try:
            self.ser.write(data)
            self.ser.flush()
        except serial.SerialException as e:
            raise Disconnected("cannot write to port") from e

    def _read(self, n: int) -> bytes:
        try:
            return self.ser.read(n)
        except serial.SerialException as e:
            raise Disconnected("cannot read from port") from e

    def _flush_input(self):
        try:
            self.ser.reset_input_buffer()
        except serial.SerialException as e:
            raise Disconnected("board disconnected") from e

    def close(self):
        if self.ser and self.ser.is_open:
            self.ser.close()


class TcpTransport(_FrameTransport):
    """
    The same protocol, carried over TCP to the WiFi gateway.

    The gateway is a transparent bridge: whatever arrives on the socket
    is written to USART1 and vice versa. It parses nothing, so there is
    no second implementation of the protocol to keep in step -- the
    bootloader talks to this script exactly as it does over a wire, and
    the ESP32 only moves bytes.

    Consequences worth knowing. TCP already guarantees delivery and
    ordering, so the frame CRC and the sequence numbers are redundant
    over this hop -- but they are not redundant over the UART hop from
    the gateway to the STM32, which is where corruption actually
    happens. Keeping them end to end is what makes the two links
    behave identically.
    """

    name = "tcp"

    def __init__(self, host: str, port: int = 3333,
                 timeout: float = 1.0, verbose: bool = False):
        super().__init__(timeout, verbose)
        try:
            self.sock = socket.create_connection((host, port), timeout=5.0)
        except OSError as e:
            raise TransportError(f"cannot connect to {host}:{port}: {e}")
        self.sock.settimeout(timeout)

    def _write(self, data: bytes):
        try:
            self.sock.sendall(data)
        except OSError as e:
            raise Disconnected("cannot write to socket") from e

    def _read(self, n: int) -> bytes:
        try:
            return self.sock.recv(n)
        except socket.timeout:
            return b""
        except OSError as e:
            raise Disconnected("cannot read from socket") from e

    def _flush_input(self):
        # Drain whatever is pending without blocking.
        self.sock.settimeout(0.01)
        try:
            while self.sock.recv(4096):
                pass
        except OSError:
            pass
        finally:
            self.sock.settimeout(self.timeout)

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass
