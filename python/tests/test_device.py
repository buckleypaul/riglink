import pytest
import riglink
from riglink._wire import DEFAULT_SENTINEL
from riglink.exceptions import RiglinkAssertError, RiglinkProtocolError, RiglinkSignatureError, RiglinkTimeout
from riglink.transport import LoopbackTransport
from tests._fakedev import FakeDevice, _FakeArgTooLong, _FakeAssert


@pytest.fixture
def fd():
    return FakeDevice()


@pytest.fixture
def dev(fd):
    d = riglink.connect(fd.transport, wait_ready=True, ready_timeout=1.0)
    yield d
    d.close()


def _check_thing_handler(a):
    """Simulate a RIG_ASSERT: raise _FakeAssert if arg != 1."""
    if int(a) != 1:
        raise _FakeAssert("a == 1")
    return int(a) * 10


def _setup_cmds(fd):
    fd.command("add2", "int", ["int", "int"])(lambda a, b: int(a) + int(b))
    fd.command("do_reboot", "void", [])(lambda: None)
    fd.command("dump_state", "void", [])(lambda: {"uptime": 1234, "mode": "idle"})
    fd.command("check_thing", "int", ["int"])(_check_thing_handler)


def test_connect_handshakes_and_lists_commands(fd):
    _setup_cmds(fd)
    d = riglink.connect(fd.transport, ready_timeout=1.0)
    assert "add2" in d.command_names
    assert d.signature("add2") == {"name": "add2", "ret": "int", "args": ["int", "int"]}
    d.close()


def test_call_returns_ret_dict(fd):
    _setup_cmds(fd)
    d = riglink.connect(fd.transport, ready_timeout=1.0)
    assert d.add2(2, 3) == {"ret": 5}
    d.close()


def test_call_void_returns_ret_none(fd, dev):
    fd.command("do_reboot", "void", [])(lambda: None)
    # registered after connect → not in cached signatures; re-handshake
    dev.list_commands(refresh=True)
    assert dev.do_reboot() == {"ret": None}


def test_call_void_with_emitted_fields_returns_extras_alongside_ret(fd, dev):
    fd.command("dump_state", "void", [])(lambda: {"uptime": 1234, "mode": "idle"})
    dev.list_commands(refresh=True)
    assert dev.dump_state() == {"ret": None, "uptime": 1234, "mode": "idle"}


def test_unknown_attribute_raises_attributeerror(dev):
    with pytest.raises(AttributeError):
        _ = dev.totally_unknown


def test_bad_arity_raises_signature_error(fd):
    fd.command("add2", "int", ["int", "int"])(lambda a, b: int(a) + int(b))
    d = riglink.connect(fd.transport, ready_timeout=1.0)
    with pytest.raises(RiglinkSignatureError):
        d.add2(1)
    d.close()


def test_device_error_envelope_raises(fd):
    d = riglink.connect(fd.transport, ready_timeout=1.0)
    # rig.echo exists; ask for something unknown via .call-style
    with pytest.raises(RiglinkProtocolError) as ei:
        d._send("frobnicate", [], 1.0)
    assert ei.value.code == "unknown_cmd"
    d.close()


def test_arg_too_long_envelope_surfaces_code_and_details(fd):
    """An over-length `str` arg yields code 'arg_too_long' with arg/got/max — the
    generic RiglinkProtocolError path carries it, no special host handling needed."""
    def _setkey(key):
        raise _FakeArgTooLong(0, len(key), 63)

    fd.command("setkey", "void", ["str"])(_setkey)
    d = riglink.connect(fd.transport, wait_ready=True, ready_timeout=1.0)
    with pytest.raises(RiglinkProtocolError) as ei:
        d.setkey("a" * 64)
    assert ei.value.code == "arg_too_long"
    assert ei.value.details["arg"] == 0
    assert ei.value.details["got"] == 64
    assert ei.value.details["max"] == 63
    assert ei.value.cmd == "setkey"
    d.close()


def test_assert_failure_raises_assert_error(fd):
    _setup_cmds(fd)
    d = riglink.connect(fd.transport, ready_timeout=1.0)
    assert d.check_thing(1) == {"ret": 10}
    with pytest.raises(RiglinkAssertError) as ei:
        d.check_thing(2)
    assert ei.value.code == "assert"
    d.close()


def test_expect_event(fd):
    d = riglink.connect(fd.transport, ready_timeout=1.0)
    fd.emit_event("rx_done", len=12)
    ev = d.expect_event("rx_done", timeout=1.0, len=12)
    assert ev == {"event": "rx_done", "len": 12}
    with pytest.raises(RiglinkTimeout):
        d.expect_event("never", timeout=0.1)
    d.close()


def test_reset_via_command_and_ready(fd):
    d = riglink.connect(fd.transport, ready_timeout=1.0)
    d.reset(timeout=1.0, ready_timeout=1.0)
    assert fd.reset_count == 1
    d.close()


def _recording_device(**kwargs):
    """A Device over a loopback that records every outbound line and answers each
    write with a canned response so _send/call_raw don't block. Returns
    (device, sent) where `sent` is the list of decoded request lines."""
    sent: list[str] = []

    def on_write(data: bytes) -> None:
        sent.append(data.decode().rstrip())
        t.feed(DEFAULT_SENTINEL + b'{"cmd":"x","ret":null}\n')

    t = LoopbackTransport(on_write)
    dev = riglink.Device(t, default_timeout=1.0, **kwargs)
    return dev, sent


def test_shell_root_transforms_outbound_command_names():
    dev, sent = _recording_device(shell_root="rig")
    try:
        dev._send("add", [2, 3], 1.0)
        dev._send("scale", [3, 2.5], 1.0)
        dev._send("g_counter.set", [42], 1.0)
        dev._send("rig.list", [], 1.0)
        dev.call_raw("rig.reset")
    finally:
        dev.close()
    assert sent == [
        "rig add 2 3",
        "rig scale 3 2.5",
        "rig g_counter.set 42",
        "rig list",
        "rig reset",
    ]


def test_shell_root_none_leaves_names_unchanged():
    dev, sent = _recording_device()  # shell_root defaults to None
    try:
        dev._send("add", [2, 3], 1.0)
        dev._send("rig.list", [], 1.0)
        dev.call_raw("rig.reset")
    finally:
        dev.close()
    assert sent == ["add 2 3", "rig.list", "rig.reset"]


def test_shell_root_leaves_write_line_raw():
    dev, sent = _recording_device(shell_root="rig")
    try:
        dev.write_line("rig add 2 3")  # REPL escape hatch: user prefixes themselves
    finally:
        dev.close()
    assert sent == ["rig add 2 3"]


def test_transcript_records_traffic(fd):
    _setup_cmds(fd)
    d = riglink.connect(fd.transport, ready_timeout=1.0)
    d.add2(1, 2)
    tx = d.transcript
    # transcript stores send lines as-is and received dicts as Python repr
    assert "send: add2 1 2" in tx and "'ret': 3" in tx
    d.close()
