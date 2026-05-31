"""The riglink wire codec: encode a call to a request line; classify a device
output line. Pure functions, no I/O."""
from __future__ import annotations

import base64
import json
import math
from typing import Any, Iterable

DEFAULT_SENTINEL = b"\x1eRIG "

# Scalar type keywords, mirroring the keyword dictionaries (RIG_CTYPE_* etc.) in
# include/riglink_pp.h. Keep these in sync with that header when keywords change.
INT_KEYWORDS = frozenset({
    "int", "unsigned", "char", "int8_t", "int16_t", "int32_t", "int64_t",
    "uint8_t", "uint16_t", "uint32_t", "uint64_t", "size_t", "intptr_t",
    "uintptr_t", "ptrdiff_t",
})
FLOAT_KEYWORDS = frozenset({"float", "double"})


def _needs_quoting(s: str) -> bool:
    if s == "":
        return True
    if s[0] == "#":
        return True
    return any(c.isspace() or c == '"' for c in s)


def encode_token(value: Any) -> str:
    """Encode one Python value as a single request token."""
    if isinstance(value, bool):          # MUST precede int (bool is an int subclass)
        return "true" if value else "false"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        if not math.isfinite(value):
            raise ValueError(f"cannot encode non-finite float {value!r} on the wire")
        return repr(value)               # round-trippable
    if isinstance(value, (bytes, bytearray)):
        value = base64.b64encode(bytes(value)).decode("ascii")  # firmware-side: decode in a custom RIG_FN
    if not isinstance(value, str):
        value = str(value)
    for c in value:
        if ord(c) < 0x20 or ord(c) == 0x7F:
            raise ValueError(
                f"string token contains control character {c!r}; "
                "not representable on the newline-delimited wire"
            )
    if _needs_quoting(value):
        escaped = value.replace("\\", "\\\\").replace('"', '\\"')
        return f'"{escaped}"'
    return value


def encode_call(name: str, args: Iterable[Any] = ()) -> bytes:
    """Encode a command invocation as a newline-terminated request line."""
    parts = [name] + [encode_token(a) for a in args]
    return (" ".join(parts) + "\n").encode("utf-8")


def classify(line: bytes, sentinel: bytes = DEFAULT_SENTINEL) -> tuple[str, Any]:
    """Classify one device output line (without its trailing newline).

    Returns (kind, payload):
        ("response", dict)   — a command response or error envelope (has "cmd")
        ("event", dict)      — an asynchronous event (has "event")
        ("log", str)         — a rig_log() passthrough line
        ("console", str)     — non-sentinel device console output (verbatim)
        ("malformed", str)   — a sentinel line whose JSON did not parse / had no known key
    """
    line = line.rstrip(b"\r\n")
    if not line.startswith(sentinel):
        return ("console", line.decode("utf-8", errors="replace"))
    body = line[len(sentinel):]
    try:
        obj = json.loads(body)
    except json.JSONDecodeError:
        return ("malformed", body.decode("utf-8", errors="replace"))
    if not isinstance(obj, dict):
        return ("malformed", body.decode("utf-8", errors="replace"))
    if "cmd" in obj:
        return ("response", obj)
    if "event" in obj:
        return ("event", obj)
    if "log" in obj:
        return ("log", str(obj["log"]))
    return ("malformed", body.decode("utf-8", errors="replace"))
