"""riglink — host-driven firmware testing over serial."""
from __future__ import annotations

from .exceptions import (
    RiglinkAssertError,
    RiglinkError,
    RiglinkProtocolError,
    RiglinkSignatureError,
    RiglinkTimeout,
)

__version__ = "0.1.0"
__all__ = [
    "__version__",
    "connect",
    "Device",
    "RiglinkError",
    "RiglinkSignatureError",
    "RiglinkProtocolError",
    "RiglinkAssertError",
    "RiglinkTimeout",
]


def connect(*args, **kwargs):
    """Open a serial link to a riglink device, handshake, and return a Device.
    (Defined in riglink.device; re-exported here. Imported lazily to keep
    `import riglink` cheap and avoid importing pyserial until needed.)"""
    from .device import connect as _connect

    return _connect(*args, **kwargs)


def __getattr__(name: str):
    if name == "Device":
        from .device import Device

        return Device
    raise AttributeError(name)
