from riglink._wire import DEFAULT_SENTINEL
from tests._fakedev import FakeDevice


def test_loopback_roundtrip_via_fakedevice():
    fd = FakeDevice()
    # FakeDevice emits a ready event on construction; drain it
    first = fd.transport.read()
    assert first.startswith(DEFAULT_SENTINEL) and b'"event":"ready"' in first

    fd.transport.write(b"rig.echo a b\n")
    resp = fd.transport.read()
    assert resp == DEFAULT_SENTINEL + b'{"cmd":"rig.echo","ret":["a","b"]}\n'


def test_loopback_unknown_command():
    fd = FakeDevice()
    fd.transport.read()  # ready
    fd.transport.write(b"nope\n")
    assert b'"error":{"code":"unknown_cmd"}' in fd.transport.read()


def test_loopback_event_injection():
    fd = FakeDevice()
    fd.transport.read()  # ready
    fd.emit_event("rx_done", len=12)
    assert fd.transport.read() == DEFAULT_SENTINEL + b'{"event":"rx_done","len":12}\n'
