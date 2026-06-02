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
                 "arg_too_long", "assert", "internal", ...).
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
    """No response (or no matching event) within the timeout."""
