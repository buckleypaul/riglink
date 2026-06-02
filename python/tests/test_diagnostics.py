"""Tests for "loud timeouts": a RiglinkTimeout should carry recent device output
(console / log / malformed lines) so a silent firmware-side failure is diagnosable
from the exception alone, and Device should expose line-kind counters/diagnostics."""
import warnings

import pytest
import riglink
from riglink._wire import DEFAULT_SENTINEL
from riglink.device import RECENT_LINES_CAP
from riglink.exceptions import RiglinkTimeout
from riglink.transport import LoopbackTransport


def _silent_device():
    """A Device over a loopback whose sink never answers writes — every call
    therefore times out. We can still feed console/log/malformed lines in."""
    t = LoopbackTransport(lambda data: None)
    dev = riglink.Device(t, default_timeout=0.2)
    return dev, t


def test_timeout_attaches_recent_console_output():
    dev, t = _silent_device()
    try:
        # A shell-style plain-text rejection sitting on the wire.
        t.feed(b"add: wrong parameter count\n")
        with pytest.raises(RiglinkTimeout) as ei:
            dev._send("add", [1], 0.2)
        exc = ei.value
        # The plain-text line must be visible in both the message and attributes.
        assert "add: wrong parameter count" in str(exc)
        assert any("wrong parameter count" in line for line in exc.recent_console)
    finally:
        dev.close()


def test_timeout_attaches_recent_logs():
    dev, t = _silent_device()
    try:
        t.feed(DEFAULT_SENTINEL + b'{"log":"sensor init failed"}\n')
        with pytest.raises(RiglinkTimeout) as ei:
            dev._send("read_sensor", [], 0.2)
        exc = ei.value
        assert any("sensor init failed" in line for line in exc.recent_logs)
        assert "sensor init failed" in str(exc)
    finally:
        dev.close()


def test_timeout_reports_malformed_lines():
    dev, t = _silent_device()
    try:
        # A corrupted sentinel line classifies as malformed.
        t.feed(DEFAULT_SENTINEL + b'{"cmd":"add","re\n')
        with pytest.raises(RiglinkTimeout) as ei:
            dev._send("add", [1, 2], 0.2)
        exc = ei.value
        assert exc.malformed_count >= 1
        # Malformed lines are surfaced (they land in console as <malformed: ...>).
        assert any("malformed" in line for line in exc.recent_console)
    finally:
        dev.close()


def test_timeout_caps_attached_lines():
    dev, t = _silent_device()
    try:
        for i in range(200):
            t.feed(f"noise line {i}\n".encode())
        with pytest.raises(RiglinkTimeout) as ei:
            dev._send("add", [1, 2], 0.3)
        exc = ei.value
        # Capped to RECENT_LINES_CAP — not the full 200 lines.
        assert len(exc.recent_console) <= RECENT_LINES_CAP
        # The most recent lines are the ones kept.
        assert any("noise line 199" in line for line in exc.recent_console)
    finally:
        dev.close()


def test_timeout_backward_compatible_plain_message():
    # RiglinkTimeout must still be constructible with just a message.
    exc = RiglinkTimeout("no response")
    assert str(exc) == "no response"
    assert exc.recent_console == []
    assert exc.recent_logs == []
    assert exc.malformed_count == 0


def test_diagnostics_property_counts_line_kinds():
    dev, t = _silent_device()
    try:
        t.feed(b"plain console line\n")
        t.feed(DEFAULT_SENTINEL + b'{"log":"hello"}\n')
        t.feed(DEFAULT_SENTINEL + b'{"cmd":"x","re\n')  # malformed
        # Drain through a (timing-out) call so the reader has surely processed them.
        with pytest.raises(RiglinkTimeout):
            dev._send("add", [1, 2], 0.2)
        diag = dev.diagnostics
        assert diag["console"] >= 1
        assert diag["log"] >= 1
        assert diag["malformed"] >= 1
    finally:
        dev.close()


def test_malformed_line_emits_warning():
    dev, t = _silent_device()
    try:
        with warnings.catch_warnings(record=True) as caught:
            warnings.simplefilter("always")
            t.feed(DEFAULT_SENTINEL + b'{"cmd":"x","re\n')  # malformed
            # Spin the reader until it processes the line.
            with pytest.raises(RiglinkTimeout):
                dev._send("add", [1, 2], 0.2)
        assert any("malformed" in str(w.message).lower() for w in caught), [str(w.message) for w in caught]
    finally:
        dev.close()
