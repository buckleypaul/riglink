# Handoff: unify the host-side response return shape

**Date:** 2026-05-12
**Status:** RESOLVED 2026-05-12 — design + implementation landed; see
`docs/superpowers/specs/2026-05-12-ret-return-shape.md`. Decision: `"ret"` goes
on the wire (not just a host re-key); void is `"ret":null`; `_interpret` always
returns `{"ret": <value>, **extras}`.
**Area:** Python host library (`python/riglink/device.py`, `_interpret`), wire protocol (firmware response envelope), and everything downstream (CLI, pytest plugin, stubs, docs).

## The issue

`Device._interpret()` (in `python/riglink/device.py`) collapses a firmware
response into one of **four** different return shapes depending on the *content*
of that particular response, not on the command:

| Firmware return | Extra top-level fields emitted? | `dev.f()` evaluates to |
|---|---|---|
| non-void | no | the bare result value |
| non-void | yes | `{"result": <value>, **extras}` |
| void (`"result":{}`) | no | `None` |
| void | yes | a dict of just the extras (no `result` key at all) |

Context: a `RIG_FN` body can emit extra top-level fields via `rig_emit()` (e.g.
`{"cmd":"status","uptime_ms":1234,"result":{}}`), and those extras can accompany
either a void or a non-void return. The current code keeps the bare-value case
ergonomic at the cost of an unpredictable shape: the same method can return an
`int` on one call and a `dict` on the next if the firmware conditionally emits a
diagnostic field (`"_dropped"`, `"_emit_overflow"`, etc.). The dict cases are
also asymmetric — non-void-with-extras puts the value under `["result"]`, but
void-with-extras has no `result` key — so there's no single uniform "unwrap"
helper.

This is currently load-bearing in at least one place: `rig.list`'s `cmds` array
arrives *as an extra field*, and `Device.handshake()` relies on getting a dict
back so it can pull `resp["cmds"]`. Any change has to keep that working (or
adjust `handshake()` accordingly).

The behavior is documented in the `_interpret` docstring and in `ARCHITECTURE.md`
§3.3 / §5-note; it's a deliberate choice, not a bug — but it's a footgun and the
asymmetry makes the code hard to evolve safely.

## What we want

Consolidate all the variants into **one** uniform manner: a response is **always**
returned as a `dict` with the return value under a single fixed key, and the key
is **`"ret"`** (short for "return" — chosen over `"result"` to keep it terse on
the wire and in user code, and to make the migration grep-able). So:

- non-void, no extras  → `{"ret": <value>}`
- non-void, with extras → `{"ret": <value>, **extras}`
- void, no extras → `{"ret": None}` (or whatever the agreed void sentinel is)
- void, with extras → `{"ret": None, **extras}`

No more "bare value vs. dict depending on content." Callers always index `["ret"]`
for the return value and find extras as sibling keys.

Scope notes (not solutions — just things the change has to touch):

- This is a breaking change to the host API (`dev.add(2,3)` stops being `5` and
  becomes `{"ret": 5}`); we're OK with that pre-1.0. Update every
  example/test/fixture: `pytest_plugin.py`, `cli.py`, `tests/integration/`,
  `python/tests/`, the C tests, and all README/ARCHITECTURE/spec snippets.
- Decide whether `"ret"` is purely a host-layer re-key of `result`, or also
  changes on the wire (firmware emitting `"ret"`); apply consistently either way.
- Decide how a void return is represented under `"ret"`.
- `Device.handshake()` reads `resp["cmds"]`; confirm that and the
  `peek`/`echo`/`reset` convenience wrappers still work under the new shape.
- Update `ARCHITECTURE.md` once this lands — at minimum §1 (wire protocol
  summary), §3.3 (the `_interpret` return-shape description), §4 (the end-to-end
  walkthrough), and the §5 closing note that currently flags this quirk; the note
  should describe the new uniform `"ret"` behavior instead of the four-shape one.

## Pointers

- `python/riglink/device.py` — `_interpret`, `_txn`, `_send`, `call_raw`, `handshake`, the built-in convenience wrappers.
- `python/riglink/_wire.py` — `classify` (only relevant if `"ret"` goes on the wire).
- `src/riglink_io.c` — `rig_io_resp_begin` / `rig_io_result_void` / `RIG_EMIT_*` (only if on the wire); `src/riglink_cmd.c` built-ins; `include/riglink.h` macros.
- `README.md` "Wire protocol summary" + Python examples; `ARCHITECTURE.md` §1, §3.3, §4, §5-note; `docs/superpowers/specs/2026-05-11-riglink-design.md`.
- Tests: `tests/test_*.c`, `python/tests/`, `tests/integration/`.
