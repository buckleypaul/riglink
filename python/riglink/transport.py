"""Byte transports for the riglink host library. A Transport is a bidirectional
byte pipe with a non-blocking-ish read; the Device layer sits on top of it."""
from __future__ import annotations

import abc
import queue
import time
from typing import Optional


class Transport(abc.ABC):
    """A bidirectional byte pipe to a riglink device."""

    @abc.abstractmethod
    def write(self, data: bytes) -> None: ...

    @abc.abstractmethod
    def read(self) -> bytes:
        """Return whatever bytes have arrived, blocking up to a short internal
        timeout. May return b"". Raises ConnectionError if the link is gone."""

    @abc.abstractmethod
    def close(self) -> None: ...

    def reopen(self) -> None:
        """Re-establish the link after a device reset (USB-CDC re-enumeration,
        etc.). Default: nothing (the link never went away)."""

    def is_alive(self) -> bool:
        return True

    def reset_device(self) -> bool:
        """Best-effort hardware reset (e.g. pulse DTR). Returns True if a reset
        signal was actually asserted. Default: not supported."""
        return False


class SerialTransport(Transport):
    def __init__(self, port: str, baud: int = 115200, *, read_timeout: float = 0.05) -> None:
        import serial  # imported here so `import riglink` doesn't need pyserial

        self._port = port
        self._baud = baud
        self._read_timeout = read_timeout
        self._serial_mod = serial
        self._ser = serial.Serial(port, baud, timeout=read_timeout)

    def write(self, data: bytes) -> None:
        try:
            self._ser.write(data)
            self._ser.flush()
        except Exception as e:  # pyserial raises various SerialException subclasses
            raise ConnectionError(f"write to {self._port} failed: {e}") from e

    def read(self) -> bytes:
        try:
            n = self._ser.in_waiting
            return self._ser.read(n if n else 1)  # the timeout=read_timeout bounds the n==0 case
        except Exception as e:
            raise ConnectionError(f"read from {self._port} failed: {e}") from e

    def close(self) -> None:
        try:
            self._ser.close()
        except Exception:
            pass

    def reopen(self) -> None:
        deadline = time.monotonic() + 5.0
        last_err: Optional[Exception] = None
        self.close()
        while time.monotonic() < deadline:
            try:
                self._ser = self._serial_mod.Serial(self._port, self._baud, timeout=self._read_timeout)
                return
            except Exception as e:  # device still re-enumerating
                last_err = e
                time.sleep(0.1)
        raise ConnectionError(f"could not reopen {self._port}: {last_err}")

    def is_alive(self) -> bool:
        return bool(self._ser and self._ser.is_open)

    def reset_device(self) -> bool:
        try:
            self._ser.dtr = False
            time.sleep(0.05)
            self._ser.dtr = True
            return True
        except Exception:
            return False


class LoopbackTransport(Transport):
    """An in-process Transport. Bytes written by the host are handed to a sink
    callback (typically a FakeDevice); bytes the sink produces are pushed back
    via .feed() and delivered by .read()."""

    def __init__(self, on_write) -> None:
        self._on_write = on_write           # callable(bytes) -> None
        self._inbox: "queue.Queue[bytes]" = queue.Queue()
        self._closed = False

    def write(self, data: bytes) -> None:
        if self._closed:
            raise ConnectionError("loopback closed")
        self._on_write(data)

    def feed(self, data: bytes) -> None:    # the sink pushes device->host bytes here
        self._inbox.put(data)

    def read(self) -> bytes:
        if self._closed:
            raise ConnectionError("loopback closed")
        try:
            return self._inbox.get(timeout=0.05)
        except queue.Empty:
            return b""

    def close(self) -> None:
        self._closed = True

    def is_alive(self) -> bool:
        return not self._closed
