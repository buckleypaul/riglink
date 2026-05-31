"""The Device proxy: dynamic command attributes built from the device's own
rig.list, a background reader thread, synchronous calls, async events, reset."""
from __future__ import annotations

import collections
import queue
import threading
import time
from typing import Any, Iterable, Optional

from ._wire import DEFAULT_SENTINEL, FLOAT_KEYWORDS, INT_KEYWORDS, classify, encode_call
from .exceptions import (
    RiglinkAssertError,
    RiglinkProtocolError,
    RiglinkSignatureError,
    RiglinkTimeout,
)
from .transport import SerialTransport, Transport


class _Call:
    def __init__(self, dev: "Device", name: str) -> None:
        self._dev = dev
        self._name = name

    def __call__(self, *args: Any, timeout: Optional[float] = None) -> dict:
        sig = self._dev._signatures.get(self._name)
        if sig is None:
            raise RiglinkSignatureError(
                f"device has no command {self._name!r}; known: {sorted(self._dev._signatures)}"
            )
        decl = sig.get("args", [])
        if decl != ["..."]:
            if len(args) != len(decl):
                raise RiglinkSignatureError(
                    f"{self._name} expects {len(decl)} argument(s) {decl}, got {len(args)}"
                )
            for i, (a, kw) in enumerate(zip(args, decl)):
                _check_arg(self._name, i, a, kw)
        return self._dev._send(self._name, args, timeout)


def _check_arg(cmd: str, i: int, value: Any, kw: str) -> None:
    ok = True
    if kw == "bool":
        ok = isinstance(value, bool)
    elif kw in INT_KEYWORDS:
        ok = isinstance(value, int) and not isinstance(value, bool)
    elif kw in FLOAT_KEYWORDS:
        ok = isinstance(value, (int, float)) and not isinstance(value, bool)
    elif kw == "str":
        ok = isinstance(value, (str, bytes, bytearray))
    if not ok:
        raise RiglinkSignatureError(f"{cmd}: argument {i} should be {kw}, got {type(value).__name__}")


class Device:
    def __init__(self, transport: Transport, *, sentinel: bytes = DEFAULT_SENTINEL,
                 default_timeout: float = 1.0, on_line=None,
                 shell_root: Optional[str] = None) -> None:
        self._t = transport
        self._sentinel = sentinel
        self._timeout = default_timeout
        self._on_line = on_line
        self._shell_root = shell_root
        self._pending: "queue.Queue[dict]" = queue.Queue()
        self._events: "collections.deque[dict]" = collections.deque()
        self._evcond = threading.Condition()
        self._logs: list[str] = []
        self._console: list[str] = []
        self._transcript: list[tuple[str, str]] = []
        self._call_lock = threading.Lock()
        self._signatures: dict[str, dict] = {}
        self._attr_map: dict[str, Optional[str]] = {}   # attr name -> command name (None if ambiguous)
        self._buf = b""
        self._stop = False
        self._start_reader()

    def _start_reader(self) -> None:
        """Create and start the background reader thread (used by __init__ and reset)."""
        self._stop = False
        self._reader = threading.Thread(target=self._read_loop, name="riglink-reader", daemon=True)
        self._reader.start()

    # -- reader thread -------------------------------------------------
    def _read_loop(self) -> None:
        while not self._stop:
            try:
                data = self._t.read()
            except ConnectionError:
                break
            if not data:
                continue
            self._buf += data
            while b"\n" in self._buf:
                line, self._buf = self._buf.split(b"\n", 1)
                kind, payload = classify(line, self._sentinel)
                if kind == "response":
                    self._transcript.append(("recv", repr(payload)))
                    self._pending.put(payload)
                elif kind == "event":
                    self._transcript.append(("recv", f"event {payload}"))
                    with self._evcond:
                        self._events.append(payload)
                        self._evcond.notify_all()
                elif kind == "log":
                    self._transcript.append(("recv", f"log: {payload}"))
                    self._logs.append(payload)
                elif kind == "console":
                    if payload:
                        self._console.append(payload)
                else:  # malformed
                    self._console.append(f"<malformed: {payload}>")
                if self._on_line is not None:
                    try:
                        self._on_line(kind, payload)
                    except Exception as e:
                        self._console.append(f"<on_line callback error: {e!r}>")

    # -- calls ---------------------------------------------------------
    def _interpret(self, resp: dict) -> dict:
        """Turn a raw response dict into the uniform host-side shape, or raise.

        Always returns a ``dict``: the firmware return value under ``"ret"``
        and any extra top-level fields the command emitted as sibling keys.

        * non-void, no extras   → ``{"ret": <value>}``
        * non-void, with extras → ``{"ret": <value>, **extras}``
        * void, no extras       → ``{"ret": None}``
        * void, with extras     → ``{"ret": None, **extras}``

        An error envelope (``"error"`` present) raises ``RiglinkAssertError``
        for code ``"assert"``, otherwise ``RiglinkProtocolError``.
        """
        if "error" in resp:
            err = resp["error"] if isinstance(resp["error"], dict) else {"code": str(resp["error"])}
            code = err.get("code", "internal")
            cmd = resp.get("cmd")
            if code == "assert":
                raise RiglinkAssertError(code, err, cmd)
            raise RiglinkProtocolError(code, err, cmd)
        out = {"ret": resp.get("ret")}
        out.update((k, v) for k, v in resp.items() if k not in ("cmd", "ret"))
        return out

    def _txn(self, line: bytes, name: str, timeout: Optional[float]) -> dict:
        """One synchronous call transaction: drain stale responses, write the
        request line, wait for the reply, interpret it (or raise on timeout).
        Returns the response dict (``{"ret": <value>, **extras}``)."""
        timeout = self._timeout if timeout is None else timeout
        with self._call_lock:
            # discard any stale response (shouldn't happen with one-in-flight)
            try:
                while True:
                    self._pending.get_nowait()
            except queue.Empty:
                pass
            self._transcript.append(("send", line.decode().rstrip()))
            self._t.write(line)
            try:
                resp = self._pending.get(timeout=timeout)
            except queue.Empty:
                raise RiglinkTimeout(f"no response to {name!r} within {timeout}s")
        return self._interpret(resp)

    def _wire_name(self, name: str) -> str:
        """Map a canonical command name to the token(s) the device's frontend
        expects on the wire. With the poll backend (``shell_root`` is ``None``)
        this is the identity. With the shell backend the command is a subcommand
        of a root ``rig`` command, so prepend the root token and strip the
        leading ``rig.`` that ``rig_dyn_get`` drops from the shell syntax
        (e.g. ``add`` -> ``rig add``, ``rig.list`` -> ``rig list``)."""
        if not self._shell_root:
            return name
        tok = name.removeprefix(self._shell_root + ".")
        return f"{self._shell_root} {tok}"

    def _send(self, name: str, args: Iterable[Any], timeout: Optional[float]) -> dict:
        return self._txn(encode_call(self._wire_name(name), args), name, timeout)

    def call_raw(self, name: str, *tokens: str, timeout: Optional[float] = None) -> dict:
        """Send a raw command by name with pre-stringified tokens; return the
        response dict (``{"ret": <value>, **extras}``).

        Unlike the dynamic attribute API, no type-checking or encoding is applied —
        tokens are joined as-is and written to the transport verbatim. The name is
        still routed through the shell-backend transform (see :meth:`_wire_name`).
        """
        return self._txn((" ".join([self._wire_name(name), *tokens]) + "\n").encode(), name, timeout)

    # -- dynamic attributes -------------------------------------------
    def __getattr__(self, name: str) -> Any:
        if name.startswith("_"):
            raise AttributeError(name)
        try:
            sigs = object.__getattribute__(self, "_signatures")
            amap = object.__getattribute__(self, "_attr_map")
        except AttributeError:
            raise AttributeError(name)
        target = name if name in sigs else amap.get(name)
        if target is None:
            raise AttributeError(
                f"{name!r}: not a known device command (known: {sorted(sigs)})"
            )
        return _Call(self, target)

    # -- handshake / reset --------------------------------------------
    def handshake(self) -> None:
        """Run the rig.list handshake: (re)load the device's command signatures
        and the dotted-name attribute aliases. Safe to call again to refresh."""
        resp = self._send("rig.list", [], self._timeout)
        self._signatures = {c["name"]: c for c in resp["cmds"]}
        amap: dict[str, Optional[str]] = {}
        for n in self._signatures:
            alias = n.replace(".", "_")
            if alias == n:
                continue
            amap[alias] = None if alias in amap else n
        self._attr_map = amap

    def _handshake(self) -> None:
        """Backwards-compatible alias for :meth:`handshake`."""
        self.handshake()

    def write_line(self, text: str) -> None:
        """Write a raw line to the transport (REPL-style). A single trailing
        newline in ``text`` is stripped; exactly one ``"\\n"`` is appended. The
        line is recorded in the transcript as a ``("send", ...)`` entry."""
        line = text[:-1] if text.endswith("\n") else text
        self._transcript.append(("send", line))
        self._t.write((line + "\n").encode("utf-8"))

    def reset(self, *, timeout: Optional[float] = None, reconnect: bool = True,
              refresh: bool = False, ready_timeout: float = 3.0) -> None:
        timeout = self._timeout if timeout is None else timeout
        try:
            self._send("rig.reset", [], timeout)
        except (RiglinkTimeout, RiglinkProtocolError, ConnectionError):
            pass  # the device may reboot before/while replying
        if reconnect:
            # if the transport died (USB-CDC re-enum), reopen and restart the reader
            if not self._t.is_alive():
                self._stop = True
                self._reader.join(timeout=1.0)
                self._t.reopen()
                self._buf = b""
                self._start_reader()
            # wait for a fresh ready event, best-effort
            try:
                self.expect_event("ready", timeout=ready_timeout)
            except RiglinkTimeout:
                pass
        if refresh or not self._signatures:
            self._handshake()

    # -- events / introspection ---------------------------------------
    def expect_event(self, name: str, timeout: Optional[float] = None, **match: Any) -> dict:
        timeout = self._timeout if timeout is None else timeout
        deadline = time.monotonic() + timeout
        with self._evcond:
            while True:
                for i, ev in enumerate(self._events):
                    if ev.get("event") == name and all(ev.get(k) == v for k, v in match.items()):
                        del self._events[i]
                        return ev
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise RiglinkTimeout(f"no {name!r} event within {timeout}s")
                self._evcond.wait(remaining)

    def drain_events(self) -> list[dict]:
        with self._evcond:
            out = list(self._events)
            self._events.clear()
            return out

    @property
    def events(self) -> list[dict]:
        with self._evcond:
            return list(self._events)

    @property
    def logs(self) -> list[str]:
        return list(self._logs)

    @property
    def console(self) -> list[str]:
        return list(self._console)

    @property
    def command_names(self) -> list[str]:
        return sorted(self._signatures)

    def signature(self, name: str) -> dict:
        return dict(self._signatures[name])

    def list_commands(self, *, refresh: bool = False) -> dict[str, dict]:
        if refresh:
            self._handshake()
        return {k: dict(v) for k, v in self._signatures.items()}

    @property
    def transcript(self) -> str:
        return "\n".join(f"{d:>4}: {t}" for d, t in self._transcript)

    def clear_buffers(self) -> None:
        with self._evcond:
            self._events.clear()
        self._logs.clear()
        self._console.clear()
        self._transcript.clear()

    # -- convenience wrappers for built-ins ---------------------------
    # These unwrap ["ret"]; the dynamic attribute API and call_raw() don't.
    def echo(self, *args: Any, timeout: Optional[float] = None) -> list:
        return self._send("rig.echo", args, timeout)["ret"]

    def peek(self, addr: int, n: int, timeout: Optional[float] = None) -> str:
        if "rig.peek" not in self._signatures:
            raise RiglinkSignatureError("device built without CONFIG_RIGLINK_MEM_ACCESS")
        return self._send("rig.peek", (addr, n), timeout)["ret"]

    def poke(self, addr: int, val: int, timeout: Optional[float] = None) -> None:
        if "rig.poke" not in self._signatures:
            raise RiglinkSignatureError("device built without CONFIG_RIGLINK_MEM_ACCESS")
        self._send("rig.poke", (addr, val), timeout)

    # -- lifecycle -----------------------------------------------------
    def close(self) -> None:
        self._stop = True
        try:
            self._t.close()
        finally:
            self._reader.join(timeout=1.0)

    def __enter__(self) -> "Device":
        return self

    def __exit__(self, *exc) -> None:
        self.close()


def connect(port_or_transport, baud: int = 115200, *, sentinel: bytes = DEFAULT_SENTINEL,
            timeout: float = 1.0, wait_ready: bool = True, ready_timeout: float = 2.0,
            shell_root: Optional[str] = None) -> Device:
    """Open a riglink link, perform the rig.list handshake, return a Device.

    ``shell_root`` targets the Zephyr shell backend: pass the root command token
    (``"rig"``) so outbound calls are routed as its subcommands (the handshake
    that runs here picks it up). Leave it ``None`` (default) for the poll backend.
    """
    if isinstance(port_or_transport, str):
        transport: Transport = SerialTransport(port_or_transport, baud)
    elif isinstance(port_or_transport, Transport):
        transport = port_or_transport
    else:
        raise TypeError("connect() needs a port string or a Transport")
    dev = Device(transport, sentinel=sentinel, default_timeout=timeout, shell_root=shell_root)
    if wait_ready:
        try:
            dev.expect_event("ready", timeout=ready_timeout)
        except RiglinkTimeout:
            pass  # device may already be up and won't re-emit ready
    try:
        dev._handshake()
    except Exception:
        dev.close()
        raise
    return dev
