"""Shell-backend-only assertions, layered on top of the shared test_echo.py set.

These run only for the `echo_shell` param (the `shell_dev` fixture skips on the
poll backend). They exercise behavior unique to zephyr/shell_backend.c: command
routing through Zephyr's shell, output through shell_fprintf(), and the
rig_io_lock that keeps ISR-deferred event lines from splicing into responses.
"""
import time

import pytest
from riglink.exceptions import (
    RiglinkAssertError,
    RiglinkProtocolError,
    RiglinkSignatureError,
    RiglinkTimeout,
)

# Mirror test_echo.py's expectation: echo_shell exposes the same surface.
_SHARED_COMMANDS = (
    "add", "scale", "board_info", "g_counter.get", "g_counter.set",
    "check_even", "rig.list", "rig.reset",
    # long-string / non-trivial handlers (see samples/echo_shell/src/main.c)
    "echo_str", "hash_str",
)


def _fnv1a(s: str) -> int:
    """Reference 32-bit FNV-1a, matching hash_str() in the sample firmware."""
    h = 2166136261
    for b in s.encode():
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


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


# --- long-string / non-trivial handler coverage ----------------------------
#
# The commands above are all short, no-str, and trivial, so they never exercise
# a long str arg (RX ring-buffer pressure, the rig_dyn_get path, JSON-escaped
# str emission) or a handler that does real work on the shell thread's stack.
# These tests drive the echo_str / hash_str commands added to the sample.

def test_shell_echo_str_short(shell_dev):
    # Baseline: a short string round-trips under `echo`, with the byte length as
    # a sibling field. `ret` is null because echo_str is a void RIG_FN (a `str`
    # return type is unsupported, so the echo rides an emitted field).
    assert shell_dev.echo_str("hello") == {"ret": None, "echo": "hello", "len": 5}


def test_shell_echo_str_long_roundtrip(shell_dev):
    # A long argument (near CONFIG_RIGLINK_LINE_BUF_SIZE=200, after the "rig
    # echo_str " prefix) must survive the IRQ-driven RX ring, the tokenizer,
    # rig_parse_str(), and JSON-escaped emission unchanged. This is the case
    # that would have caught the rig_dyn_get bug and the RX-buffer issue, since
    # the whole argument is echoed back in the response and compared.
    s = "k" * 160
    assert shell_dev.echo_str(s) == {"ret": None, "echo": s, "len": 160}


def test_shell_echo_str_with_quotes(shell_dev):
    # Spaces force the host to quote the token; embedded double-quotes must
    # survive the shell tokenizer's unescaping and riglink's JSON re-escaping.
    # (Lone backslashes are intentionally not tested: a single literal '\' is
    # ambiguous between the host codec and Zephyr's shell tokenizer; the wire
    # contract only promises round-tripping for the escapes the host emits.)
    s = 'a b "c" d'
    assert shell_dev.echo_str(s) == {"ret": None, "echo": s, "len": len(s.encode())}
    # A value that is itself entirely quote-looking exercises a different
    # tokenizer quoting boundary (leading/trailing double-quotes in the content).
    q = '"quoted"'
    assert shell_dev.echo_str(q) == {"ret": None, "echo": q, "len": len(q.encode())}


def test_shell_hash_str_non_trivial_handler(shell_dev):
    # hash_str does real work (a 32-bit FNV-1a over the arg) and does NOT echo
    # the input, so a correct hash proves the long arg arrived intact without
    # the reply carrying it. Run a long arg to keep stack/RX pressure on.
    s = "the quick brown fox jumps over the lazy dog " * 3  # 132 bytes
    s = s[:160]
    assert shell_dev.hash_str(s) == {"ret": _fnv1a(s), "len": len(s.encode())}


def test_shell_wrong_argcount_is_clean_error(shell_dev):
    # Failure mode (a): garbage / wrong parameter count to a KNOWN command. We go
    # through `_send` (not the typed `shell_dev.echo_str(...)` proxy) on purpose:
    # the proxy guards arg arity client-side from the rig.list signature and
    # would raise before anything hit the wire (see the next test), whereas
    # `_send` encodes the tokens straight onto the wire — the same path the
    # unknown-command test uses. That puts the FIRMWARE-side check under test: a
    # known command reaches rig_dispatch -> the trampoline's argc check -> an
    # `arg_count` error envelope (RiglinkProtocolError), unlike an unknown
    # command (which Zephyr's shell rejects with a timeout). Either way the link
    # must stay clean.
    shell_dev.clear_buffers()
    with pytest.raises(RiglinkProtocolError) as ei:
        shell_dev._send("echo_str", ["a", "b", "c"], 1.0)  # echo_str expects 1 arg
    assert ei.value.code == "arg_count", ei.value.code
    # No torn sentinel line, and the link still works afterwards.
    assert [c for c in shell_dev.console if c.startswith("<malformed:")] == []
    assert shell_dev.echo_str("ok") == {"ret": None, "echo": "ok", "len": 2}


def test_shell_host_side_argcount_guard(shell_dev):
    # The typed proxy also guards arg count client-side from the rig.list
    # signature, so a wrong count raises before anything hits the wire.
    with pytest.raises(RiglinkSignatureError):
        shell_dev.echo_str("a", "b")
