"""Exceptions raised by the riglink host library."""
from __future__ import annotations


class RiglinkError(Exception):
    """Base class for all riglink errors."""


class RiglinkSignatureError(RiglinkError):
    """Raised client-side when a call does not match the device's known signature
    (unknown command, wrong arity, or an argument of the wrong type) — before
    anything is sent over the link."""


class RiglinkProtocolError(RiglinkError):
    """Raised when the device returns an error envelope.

    Attributes:
        code:    the error code string ("unknown_cmd", "arg_count", "bad_args",
                 "assert", "internal", ...).
        details: the rest of the envelope's "error" object (a dict).
        cmd:     the command name the device echoed, if any.
    """

    def __init__(self, code: str, details: dict | None = None, cmd: str | None = None) -> None:
        self.code = code
        self.details = details or {}
        self.cmd = cmd
        msg = f"device error {code!r}"
        if cmd:
            msg += f" from {cmd!r}"
        if self.details:
            msg += f": {self.details}"
        super().__init__(msg)


class RiglinkAssertError(RiglinkProtocolError):
    """A RIG_ASSERT in a RIG_FN body failed on the device."""


class RiglinkTimeout(RiglinkError):
    """No response (or no matching event) within the timeout.

    Four distinct firmware-side root causes (an unknown command, a crash before
    the response, a corrupted/un-framed line, or a genuinely slow command) all
    surface here. To make those less ambiguous, a timeout raised by a command
    transaction carries a snapshot of recent device output captured by the
    reader thread:

    Attributes:
        recent_console:  the last few non-sentinel console lines (incl. any
                         ``<malformed: ...>`` markers), most recent last.
        recent_logs:     the last few ``rig_log()`` passthrough lines.
        malformed_count: total malformed sentinel lines seen on the link.
        console_count:   total console lines seen on the link.

    Backward-compatible: still constructible with just a message string, in
    which case the attributes default to empty / zero.
    """

    def __init__(self, message: str, *, recent_console=None, recent_logs=None,
                 malformed_count: int = 0, console_count: int = 0) -> None:
        self.recent_console = list(recent_console or [])
        self.recent_logs = list(recent_logs or [])
        self.malformed_count = malformed_count
        self.console_count = console_count
        msg = message
        if self.malformed_count:
            msg += (f"\n  [{self.malformed_count} malformed line(s) seen — "
                    "framing may be corrupted]")
        if self.recent_logs:
            msg += "\n  recent device logs:"
            msg += "".join(f"\n    {line}" for line in self.recent_logs)
        if self.recent_console:
            msg += "\n  recent device console:"
            msg += "".join(f"\n    {line}" for line in self.recent_console)
        super().__init__(msg)
