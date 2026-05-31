"""Host-side pytest template for a riglink-instrumented target.

Run with the serial port that the device is on, e.g.:

    python -m pytest test_template.py --riglink-port /dev/ttyACM0

The `dev` fixture is provided by riglink's pytest plugin (installed with the
`riglink` pip package). It connects, runs the rig.list handshake, and creates a
typed proxy method per exposed command. By default it calls dev.reset() before
each test (override with --riglink-reset or @pytest.mark.riglink).

Remember: every call returns a dict {"ret": <value>, **extras}, never a bare
value. `ret` is None for void functions.
"""
import pytest


def test_add(dev):
    assert dev.add(2, 3) == {"ret": 5}


def test_exposed_variable_roundtrip(dev):
    assert dev.g_counter_set(42) == {"ret": None}   # void setter
    assert dev.g_counter_get() == {"ret": 42}


def test_assert_surfaces_as_error(dev):
    # RIG_ASSERT(arg1 != 0) failing should raise host-side, not crash the target.
    with pytest.raises(Exception):
        dev.safe_div(1, 0)


def test_extra_fields_are_siblings_of_ret(dev):
    # A RIG_FN that calls rig_emit("uptime_ms", ...) returns them alongside ret.
    resp = dev.status()
    assert resp["ret"] is None
    assert "uptime_ms" in resp


def test_event(dev):
    # Wait for a firmware-emitted event (RIG_EMIT_EVENT / _FROM_ISR).
    ev = dev.expect_event("tick", timeout=2.0)
    assert ev["event"] == "tick"


# Multi-device example (only if multiple --riglink-port flags are passed):
# def test_multi(riglink_devices):
#     for port, d in riglink_devices.items():
#         assert d.add(1, 1) == {"ret": 2}
