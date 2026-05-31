# Design spec: one uniform host-side response shape (`"ret"`)

**Date:** 2026-05-12
**Status:** accepted — implemented this date
**Supersedes:** the four-shape `_interpret` behavior described in
`docs/handoffs/2026-05-12-interpret-return-shape.md`, `ARCHITECTURE.md` §1/§3.3,
and `docs/superpowers/specs/2026-05-11-riglink-design.md` (wire-protocol table).

## Problem

`Device._interpret()` collapsed a firmware response into one of *four* shapes
depending on the response's *content* (void vs. non-void × extras vs. no extras):
a bare scalar, a `{"result": <v>, **extras}` dict, `None`, or a bare `{**extras}`
dict. Same method, different return type call to call — a footgun — and the two
dict variants weren't even symmetric (one had a `"result"` key, the other didn't),
so there was no single "unwrap" rule.

## Decision

A response is **always** a `dict`. The firmware return value lives under a single
fixed key, **`"ret"`**, and any extra top-level fields emitted by the command
(`rig_emit()` from a `RIG_FN` body, diagnostic fields like `"_dropped"` /
`"_emit_overflow"`, the `"cmds"` array from `rig.list`) are sibling keys.

| firmware return | extras? | `dev.f(...)` evaluates to |
|---|---|---|
| non-void | no  | `{"ret": <value>}` |
| non-void | yes | `{"ret": <value>, **extras}` |
| void     | no  | `{"ret": None}` |
| void     | yes | `{"ret": None, **extras}` |

Callers always read `["ret"]` for the return value; extras are siblings.

### Sub-decisions

1. **`"ret"` goes on the wire, not just in the host layer.** The firmware emits
   `"ret"` as the return-value key (previously `"result"`). Keeping the wire and
   the host vocabulary identical avoids a translation layer and a naming mismatch,
   and `"ret"` is terser on a newline-delimited UART. (`"ret"` is already the key
   name used for the *return-type* string inside each `rig.list` command entry —
   `{"name": "...", "ret": "int", "args": [...]}` — so the vocabulary is now
   consistent: `ret` means "the return", at every level.)

2. **A void return is `"ret":null` on the wire** (previously `"result":{}`), and
   `{"ret": None}` on the host. `null` is the natural JSON spelling of "no value"
   and removes the `result == {}` special case from `_interpret`.

3. **Breaking change, accepted pre-1.0.** `dev.add(2, 3)` is now `{"ret": 5}`,
   not `5`. The built-in convenience wrappers (`dev.echo`, `dev.peek`) unwrap
   `["ret"]` themselves so they keep returning a bare value; `dev.poke`/`dev.reset`
   return `None`. `Device.handshake()` keeps reading `resp["cmds"]` (still a
   sibling key — unchanged). `rig call` prints the whole response dict.

4. **No backward-compat shim.** Both ends move together; `_interpret` does not
   accept the old `"result"` key. `_wire.classify()` is unchanged — it keys a
   response off `"cmd"`, never off the value key.

## Surface touched

- **Firmware C:** `include/riglink.h` (trampoline + `RIG_EXPOSE_VAR` macros emit
  `"ret"`; `rig_io_result_void` → `rig_io_ret_void`; doc comments);
  `src/riglink_io.c` (`rig_io_ret_void` emits `jcon_add_null("ret")`; comments);
  `src/riglink_cmd.c` (`rig.echo` array key, `rig.peek` hex key, `rig_io_ret_void`
  calls).
- **Firmware C tests:** `tests/test_cmd.c`, `test_e2e.c`, `test_io.c`,
  `test_macros.c`, `test_regression.c` — exact-wire-byte expectations updated;
  two `RIG_TEST` names containing `result` renamed to `ret`.
- **Host:** `python/riglink/device.py` (`_interpret`, `handshake`, `echo`/`peek`/
  `poke`, doc/return-type strings); `python/riglink/cli.py` (`rig call` prints the
  dict).
- **Host tests / fake:** `python/tests/_fakedev.py` (emit `"ret"`),
  `test_device.py`, `test_wire.py`, `test_transport.py`, `test_cli.py`.
- **Docs:** `README.md` (wire table, deviations note), `ARCHITECTURE.md`
  (§1 table + void note, §2.2 trampoline, §2.5, §3.2 step 2, §3.3 `_interpret`,
  §4 walkthrough), `docs/superpowers/specs/2026-05-11-riglink-design.md` (snippets).

## New `_interpret` contract

```python
def _interpret(self, resp: dict) -> dict:
    if "error" in resp:
        # raise RiglinkAssertError for code "assert", else RiglinkProtocolError
        ...
    out = {"ret": resp.get("ret")}
    out.update((k, v) for k, v in resp.items() if k not in ("cmd", "ret"))
    return out
```

## Verification

- `make test` — all native C suites pass with the updated wire-byte expectations.
- `python3 -m pytest` (repo root) — host + wire + plugin + CLI suites pass.
