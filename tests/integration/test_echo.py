import pytest
from riglink.exceptions import RiglinkAssertError


def test_handshake_lists_expected_commands(dev):
    names = dev.command_names
    for n in ("add", "scale", "board_info", "g_counter.get", "g_counter.set", "check_even", "rig.list", "rig.reset"):
        assert n in names, names


def test_add(dev):
    assert dev.add(2, 3) == {"ret": 5}
    assert dev.add(-10, 4) == {"ret": -6}


def test_scale(dev):
    assert dev.scale(3, 2.5) == {"ret": 7}


def test_board_info(dev):
    info = dev.board_info()
    assert info["ret"] is None and "board" in info and "uptime_ms" in info


def test_exposed_variable_roundtrip(dev):
    dev.g_counter_set(42)
    assert dev.g_counter_get() == {"ret": 42}


def test_assert_pass_and_fail(dev):
    assert dev.check_even(8) == {"ret": 4}
    with pytest.raises(RiglinkAssertError):
        dev.check_even(7)


def test_periodic_tick_event(dev):
    ev = dev.expect_event("tick", timeout=2.0)
    assert "n" in ev


def test_reset_clears_state(dev):
    dev.g_counter_set(9)
    assert dev.g_counter_get() == {"ret": 9}
    dev.reset()
    assert dev.g_counter_get() == {"ret": 0}   # rebooted -> g_counter back to its initializer


def test_unknown_command_raises(dev):
    # Both backends now surface an unknown command as a framed unknown_cmd error
    # envelope: the poll backend via rig_dispatch, the shell backend via the
    # rig_root_handler fall-through (Zephyr's shell rejects the unmatched
    # subcommand before rig_dispatch, so the root handler frames the error
    # instead of letting a plain-text shell message escape unparsed). See
    # test_echo_shell.test_shell_unknown_command_returns_framed_error.
    from riglink.exceptions import RiglinkProtocolError
    with pytest.raises(RiglinkProtocolError):
        dev._send("no_such_command", [], 1.0)
