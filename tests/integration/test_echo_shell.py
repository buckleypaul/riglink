"""Shell-backend-only assertions, layered on top of the shared test_echo.py set.

These run only for the `echo_shell` param (the `shell_dev` fixture skips on the
poll backend). They exercise behavior unique to zephyr/shell_backend.c: command
routing through Zephyr's shell, output through shell_fprintf(), and the
rig_io_lock that keeps ISR-deferred event lines from splicing into responses.
"""
import time

import pytest
from riglink.exceptions import RiglinkAssertError, RiglinkTimeout

# Mirror test_echo.py's expectation: echo_shell exposes the same surface.
_SHARED_COMMANDS = (
    "add", "scale", "board_info", "g_counter.get", "g_counter.set",
    "check_even", "rig.list", "rig.reset",
)


def test_shell_handshake_lists_shared_commands(shell_dev):
    # The rig.list handshake runs over the shell backend (rig list); the
    # canonical command names must match the poll-backend sample exactly.
    names = shell_dev.command_names
    for n in _SHARED_COMMANDS:
        assert n in names, names


def test_shell_stream_is_clean(shell_dev):
    # A batch of calls must leave nothing but JSON responses on the wire: no
    # shell prompt, command echo, or ANSI bytes (those would land on console),
    # and no malformed sentinel lines.
    shell_dev.clear_buffers()
    for i in range(20):
        assert shell_dev.add(i, 1) == {"ret": i + 1}
    assert shell_dev.console == [], shell_dev.console


def test_shell_assert_framing_survives_shell_fprintf(shell_dev):
    # An error envelope emitted byte-by-byte through shell_fprintf() must still
    # frame and parse into a RiglinkAssertError.
    assert shell_dev.check_even(8) == {"ret": 4}
    with pytest.raises(RiglinkAssertError):
        shell_dev.check_even(7)


def test_shell_event_response_interleaving(shell_dev):
    # The headline test for rig_io_lock: `tick` fires at 2 Hz from a timer ISR
    # and is flushed from the system workqueue, while we hammer commands on the
    # shell thread. The mutex must keep an event line from splicing into the
    # middle of a response. A splice would corrupt a sentinel line -> a
    # <malformed: ...> console entry, and/or a response that fails to parse
    # (surfacing as a timeout here). Run for ~3 s to span several ticks.
    # (Raising the tick rate in echo_shell/src/main.c widens the race window.)
    shell_dev.clear_buffers()
    n = 0
    deadline = time.monotonic() + 3.0
    while time.monotonic() < deadline:
        assert shell_dev.add(n, 1) == {"ret": n + 1}  # parse failure -> timeout/raise
        n += 1
    assert n > 0
    malformed = [c for c in shell_dev.console if c.startswith("<malformed:")]
    assert malformed == [], malformed
    # We should have actually observed ticks during the window, proving the
    # event path was live and concurrent with the command traffic.
    assert shell_dev.expect_event("tick", timeout=2.0)


def test_shell_arg_bearing_command_not_rejected_by_precheck(shell_dev):
    # Regression guard for the rig_dyn_get() arg-count bug: the dynamic-command
    # getter must zero entry->args, otherwise the shell reads uninitialised stack
    # for args.mandatory and runs its own argc range check, rejecting arg-bearing
    # commands with a plain "wrong parameter count" line on the console while no
    # riglink response is emitted (the host then times out). Because the value is
    # stack garbage the failure is nondeterministic, so hammer the call to make a
    # latent regression reliably visible.
    shell_dev.clear_buffers()
    for i in range(25):
        # add (2 args) and check_even (1 arg) both exercise the precheck path.
        assert shell_dev.add(i, 2) == {"ret": i + 2}
        assert shell_dev.check_even(i * 2) == {"ret": i}
    # The shell's rejection lands on the console as a non-sentinel line, so a
    # clean console proves the precheck never fired.
    assert shell_dev.console == [], shell_dev.console


def test_shell_unknown_command_rejected_by_shell(shell_dev):
    # The documented divergence from the poll backend: an unknown command never
    # reaches rig_dispatch — Zephyr's shell rejects the unmatched subcommand
    # before rig_sub_handler runs. So instead of a riglink unknown_cmd error
    # envelope (what samples/echo returns), the host gets no response at all
    # (RiglinkTimeout) plus a plain, non-sentinel shell message on the console.
    shell_dev.clear_buffers()
    with pytest.raises(RiglinkTimeout):
        shell_dev._send("no_such_command", [], 1.0)
    # The rejection must be clean: no corrupted/malformed sentinel line, and the
    # link is still usable for a normal call afterwards.
    assert [c for c in shell_dev.console if c.startswith("<malformed:")] == []
    assert shell_dev.add(2, 3) == {"ret": 5}
