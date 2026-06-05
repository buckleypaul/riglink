"""A tiny in-Python emulation of a riglink device, for host-library tests.

Wire it up:  fd = FakeDevice(); dev = riglink.Device(fd.transport)
Register commands with fd.command(name, ret, args)(handler) or use the defaults.
Inject events at any time with fd.emit_event(name, **fields)."""
from __future__ import annotations

import json
import threading
from typing import Callable

from riglink._wire import DEFAULT_SENTINEL, classify
from riglink.transport import LoopbackTransport


def _frame(obj: dict) -> bytes:
    return DEFAULT_SENTINEL + json.dumps(obj, separators=(",", ":")).encode() + b"\n"


class FakeDevice:
    def __init__(self) -> None:
        self.transport = LoopbackTransport(self._on_host_write)
        self._cmds: dict[str, tuple[str, list[str], Callable]] = {}
        self.reset_count = 0
        self._buf = b""
        self._lock = threading.Lock()
        # default built-ins
        self.command("rig.echo", "array", ["..."])(lambda *a: list(a))
        self.command("rig.reset", "void", [])(self._do_reset)
        # rig.list is special-cased in _dispatch (it needs the registry)
        self._cmds["rig.list"] = ("void", [], None)
        # emit ready once, like a freshly booted device
        self.emit_ready()

    # -- registration --------------------------------------------------
    def command(self, name: str, ret: str, args: list[str]):
        def deco(fn):
            self._cmds[name] = (ret, args, fn)
            return fn
        return deco

    # -- device -> host pushes ----------------------------------------
    def emit_event(self, name: str, **fields) -> None:
        self.transport.feed(_frame({"event": name, **fields}))

    def emit_ready(self) -> None:
        self.transport.feed(_frame({"event": "ready", "version": "0.1.0"}))

    def emit_log(self, text: str) -> None:
        self.transport.feed(_frame({"log": text}))

    # -- host -> device ------------------------------------------------
    def _on_host_write(self, data: bytes) -> None:
        with self._lock:
            self._buf += data
            while b"\n" in self._buf:
                line, self._buf = self._buf.split(b"\n", 1)
                self._dispatch(line.decode().strip())

    def _dispatch(self, line: str) -> None:
        if not line or line.startswith("#"):
            return
        parts = _tokenize(line)
        name, args = parts[0], parts[1:]
        if name == "rig.list":
            cmds = [{"name": n, "ret": r, "args": a} for n, (r, a, _) in self._cmds.items()]
            self.transport.feed(_frame({"cmd": "rig.list", "cmds": cmds, "ret": None}))
            return
        entry = self._cmds.get(name)
        if entry is None:
            self.transport.feed(_frame({"cmd": name, "error": {"code": "unknown_cmd"}}))
            return
        ret, decl, fn = entry
        if decl != ["..."] and len(args) != len(decl):
            self.transport.feed(_frame({"cmd": name, "error": {"code": "arg_count", "expected": len(decl), "got": len(args)}}))
            return
        try:
            value = fn(*args)
        except _FakeAssert as e:
            self.transport.feed(_frame({"cmd": name, "error": {"code": "assert", "file": "fake", "line": 0, "msg": str(e)}}))
            return
        except _FakeBadArg as e:
            self.transport.feed(_frame({"cmd": name, "error": {"code": "bad_args", "arg": e.idx, "expected": e.typ}}))
            return
        except _FakeArgTooLong as e:
            self.transport.feed(_frame({"cmd": name, "error": {"code": "arg_too_long", "arg": e.idx, "got": e.got, "max": e.maxlen}}))
            return
        if ret == "void":
            extras = value if isinstance(value, dict) else {}
            self.transport.feed(_frame({"cmd": name, **extras, "ret": None}))
        else:
            self.transport.feed(_frame({"cmd": name, "ret": value}))

    def _do_reset(self):
        self.reset_count += 1
        # like a real device: respond, then "reboot" (re-emit ready)
        # (the response itself is emitted by _dispatch; schedule ready after)
        self.transport.feed(_frame({"event": "ready", "version": "0.1.0"}))
        return None


class _FakeAssert(Exception):
    pass


class _FakeBadArg(Exception):
    def __init__(self, idx: int, typ: str) -> None:
        self.idx, self.typ = idx, typ
        super().__init__(f"arg {idx}: expected {typ}")


class _FakeArgTooLong(Exception):
    def __init__(self, idx: int, got: int, maxlen: int) -> None:
        self.idx, self.got, self.maxlen = idx, got, maxlen
        super().__init__(f"arg {idx}: {got} chars exceeds max {maxlen}")


def _tokenize(line: str) -> list[str]:
    """Mirror the firmware tokenizer closely enough for tests (quotes + escapes)."""
    out, i, n = [], 0, len(line)
    while i < n:
        while i < n and line[i] in " \t":
            i += 1
        if i >= n:
            break
        if line[i] == '"':
            i += 1
            buf = []
            while i < n and line[i] != '"':
                if line[i] == "\\" and i + 1 < n and line[i + 1] in '"\\':
                    buf.append(line[i + 1]); i += 2
                else:
                    buf.append(line[i]); i += 1
            i += 1  # closing quote
            out.append("".join(buf))
        else:
            j = i
            while j < n and line[j] not in " \t":
                j += 1
            out.append(line[i:j]); i = j
    return out
