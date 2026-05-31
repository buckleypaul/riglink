# riglink — design

*Status: approved design, pre-implementation. Date: 2026-05-11.*

> **Note (2026-05-12):** the device→host **response shape** below is out of date.
> The implemented wire format puts the return value under `"ret"` (not `"result"`),
> a void return is `"ret":null` (not `"result":{}`), and the host `_interpret`
> always returns a `dict` `{"ret": <value>, **extras}` rather than a
> content-dependent shape. Wherever this document writes `"result"` in a response
> envelope, read `"ret"`. See `docs/superpowers/specs/2026-05-12-ret-return-shape.md`
> and the "Deviations from the design spec" section of `README.md`.

## Summary

`riglink` is a firmware test harness. On the device, C macros expose firmware
functions (and custom test functions) so they can be called over a serial link.
On the host, a Python library — installable with `pip`/`uv`, with a `pytest`
plugin — drives that link: it calls device functions, gets their return values
back as JSON, and asserts on both return values and asynchronous device events.

The goal is host-driven firmware testing in the shape *"I call function X, I
expect Y to happen."* X is a call over serial; Y is a return value, a device
event, or a change observable through another exposed function.

Two deliverables, one repo:

- **Firmware library** (`riglink`) — a Zephyr-first C library. Output is emitted
  with [`jcon`](https://github.com/buckleypaul/jcon) (a streaming, zero-heap JSON
  emitter). Also builds with plain CMake/Makefile for native host unit tests and
  non-Zephyr MCU projects.
- **Host library** (`riglink`, Python) — a `pyserial`-based package providing a
  `Device` class, a `pytest` plugin, and a `rig` CLI.

The C API prefix is `rig_`; macros are `RIG_*`; Kconfig is `CONFIG_RIGLINK_*`;
the wire sentinel is `\x1eRIG `; built-in commands are `rig.*`.

## Repository layout

A single repo (`riglink/`):

```
include/            riglink.h (public API + RIG_* macros)
src/                riglink_proto.c, riglink_cmd.c, riglink_io.c, riglink_core.c
zephyr/             Kconfig, module CMake
CMakeLists.txt      Zephyr module entry; also a plain target for native builds
Makefile            host unit-test build (mirrors jcon's layout)
west.yml            imports zephyr + jcon
samples/echo/       Zephyr sample app + its host test file
tests/              native unit tests of the protocol layer
python/             the Python package (pyproject.toml, riglink/, tests/)
```

`jcon` is consumed via `west.yml` import for Zephyr builds and as a plain
source/header dependency for the native build.

## 1. Wire protocol

### Request (host → device)

Newline-terminated, human-typeable in a plain serial terminal:

```
NAME ARG ARG ...
```

- `NAME` — `[A-Za-z_][A-Za-z0-9_.]*`. Dots namespace commands (`rig.list`,
  `g_state.get`).
- `ARG` — whitespace-separated token. Either a bare token (no spaces) or a
  `"…"`-quoted token (escapes `\"` and `\\`) for strings or anything containing
  spaces. Numbers: decimal or `0x…` hex, optional leading sign, `.`/`e` accepted
  for floats. Booleans: `true` / `false`.
- Blank lines and lines beginning with `#` are ignored.
- Over-long input lines produce a `bad_args` (overflow) error and the parser
  resynchronises at the next `\n`.
- **One outstanding command at a time** — the host serialises calls. The device
  has no command queue.

### Response and other device → host lines

Every device → host line is prefixed with the sentinel `\x1eRIG ` (`\x1e` is
ASCII RS; configurable via `CONFIG_RIGLINK_SENTINEL`). The host classifies each
sentinel line by its top-level keys:

| Line | Example | Classified by |
| --- | --- | --- |
| Success, scalar return | `\x1eRIG {"cmd":"foo","result":0}` | has `cmd` + `result` |
| Success, extra fields | `\x1eRIG {"cmd":"dump","uptime":1234,"result":{}}` | `rig_emit()` adds top-level siblings |
| Void return | `result` is `{}` | |
| Protocol error | `\x1eRIG {"cmd":"foo","error":{"code":"bad_args","msg":"arg 1: expected int","arg":1}}` | has `error` |
| Event | `\x1eRIG {"event":"rx_done","len":12}` | has `event`, no `cmd` |
| Log passthrough | `\x1eRIG {"log":"got 3 packets"}` | has `log` |
| Anything else | `[00:00:01.234] <inf> app: ...` | no sentinel → captured verbatim as device console output |

Error codes:

- `unknown_cmd` — no command registered under that name.
- `arg_count` — wrong number of arguments (`expected`, `got`).
- `bad_args` — a token failed to parse to the declared type (`arg` index,
  `expected` type name), or the input line overflowed.
- `assert` — a `RIG_ASSERT` in an `RIG_FN` body failed (`file`, `line`, `msg`).
- `internal` — unexpected device-side failure.

A function that *returns* an error code is a success, not an error envelope:
`some_call -22` → `{"cmd":"some_call","result":-22}`.

JSON is minified by default (`CONFIG_RIGLINK_MINIFY=y`). `jcon` performs the
emitting; `riglink` prepends the sentinel once per line before `jcon_start`.

## 2. Firmware API

### Shims the application implements

```c
int  rig_putc(char c);     /* write one byte to the link; 0 on success, nonzero on error */
int  rig_getc(void);       /* return 0..255, or -1 if no byte is currently available (non-blocking) */
void rig_reset(void);      /* soft-reset / reboot the device */
```

### Lifecycle the application calls

```c
int  rig_init(void);       /* set up the parser, register built-ins; compile-time config, no params */
bool rig_run(void);        /* pump: consume available bytes, execute at most one complete command,
                              flush queued events; returns true to keep going, false after shutdown */
void rig_deinit(void);     /* stop; release the transport */
```

`rig_run()` does bounded, non-blocking work and is meant to be called repeatedly:

```c
rig_init();
while (rig_run()) { do_other_application_work(); }
rig_deinit();
```

With `CONFIG_RIGLINK_THREAD=y` on Zephyr, a dedicated thread runs the
`while (rig_run()) …` loop for you and the application calls nothing.

### Exposing existing functions

Return type first, then argument types, using a fixed keyword set. The generated
trampoline declares typed locals, parses the request tokens into them, calls the
function, and serialises the return value via `jcon_add` (whose `_Generic`
dispatch covers the supported types):

```c
int  foo(int x, int y);
void device_reboot(void);
int  parse_id(const char *s);

RIG_EXPOSE(int,  foo, int, int);     /* -> {"cmd":"foo","result":0} */
RIG_EXPOSE(void, device_reboot);     /* -> {"cmd":"device_reboot","result":{}} */
RIG_EXPOSE(int,  parse_id, str);     /* str maps to const char * */
```

Because the trampoline body expands to a literal call (`int r = foo(a0, a1);`),
a wrong, short, or long argument-type list is an ordinary compiler diagnostic at
the call site, not a runtime surprise. A `void` return is detected with a
standard preprocessor probe and emits `{}`.

Supported type keywords — every C base scalar type, named by a single token:

| Keyword(s) | C type | Wire form | JSON form |
| --- | --- | --- | --- |
| `void` | — | (return only, or the "no args" marker) | `{}` |
| `bool` | `bool` | `true` / `false` | JSON bool |
| `char` | `char` | decimal / `0x` hex (treated as a small integer, not a 1-char string) | JSON number |
| `int8_t` `int16_t` `int32_t` `int64_t` | the `<stdint.h>` typedefs | decimal / `0x` hex, optional sign | JSON number |
| `uint8_t` `uint16_t` `uint32_t` `uint64_t` | the `<stdint.h>` typedefs | decimal / `0x` hex | JSON number |
| `int` `unsigned` | `int`, `unsigned int` | decimal / `0x` hex | JSON number |
| `size_t` `ssize_t` `intptr_t` `uintptr_t` `ptrdiff_t` | as named | decimal / `0x` hex | JSON number |
| `float` `double` | `float`, `double` | decimal float | JSON number (return gated by `JCON_ENABLE_FLOAT`) |
| `str` | `const char *` | quoted token → NUL-terminated scratch buffer | JSON string |

Notes:

- Multi-word native types (`unsigned long`, `long long`, `unsigned char`, …) are
  not keywords because they are not single tokens; declare the `riglink` wrapper
  using the fixed-width `<stdint.h>` equivalent (`int64_t` for `long long`,
  `uint8_t` for `unsigned char`, and so on).
- Integer parsers parse via `strtoll` / `strtoull` and **range-check against the
  declared keyword's limits**; an out-of-range token is a `bad_args` error
  (`arg` index, `expected` type name).
- Emission widens to whatever `jcon_add_*` accepts (`int8_t` → `jcon_add_int`,
  `uint16_t` → `jcon_add_uint`, etc.) — JSON numbers are width-agnostic.
- Byte buffers are not a built-in keyword in v1 — pass a `str` and base64-decode
  it inside a custom `RIG_FN`. A `buf` keyword (built-in base64 byte buffers) is
  a deferred item.

The preprocessor machinery: per-arity argument counting, plus three keyword
dictionaries — `RIG_CTYPE_##kw` (→ C type), `RIG_PARSE_##kw` (→ token parser
function), `RIG_EMIT_##kw` (→ `jcon_add_*` wrapper, with `RIG_EMIT_void`
emitting `{}`). All keywords are single tokens so token-pasting works.

### Custom test functions

Same shape; you write the body. Arguments arrive as `arg0`, `arg1`, …:

```c
RIG_FN(int, scale, int, float) {
    return (int)(arg0 * arg1);          /* return value auto-serialised */
}

RIG_FN(void, dump_state) {
    rig_emit("uptime", k_uptime_get());
    rig_emit("mode", mode_name());
    /* -> {"cmd":"dump_state","uptime":...,"mode":"...","result":{}} */
}
```

`rig_emit(key, value)` adds a top-level sibling field to the response object
(next to `cmd` and `result`); it accepts the same value types as `jcon_add`. It
is called from `RIG_FN` bodies (an `RIG_EXPOSE` target is just an existing
function with no `riglink`-aware body to call it from).

`RIG_ASSERT(cond)` inside an `RIG_FN` body: on failure the call returns
`{"cmd":...,"error":{"code":"assert","file":...,"line":...,"msg":"cond"}}` and
the host raises `RiglinkAssertError`. This lets a test script a multi-step check
on the device and fail the host test from there.

### Exposing a variable

```c
RIG_EXPOSE_VAR(int, g_state);   /* generates g_state.get  ->  {"cmd":"g_state.get","result":<val>}
                                   and       g_state.set <value> */
```

The type keyword (given once) selects the parser and the emitter, exactly as for
function arguments/returns.

### Events

From thread context (the `riglink` thread or a workqueue — `jcon` is
single-writer and non-reentrant, so this must not run in an ISR):

```c
RIG_EMIT_EVENT("rx_done", "len", n, "crc_ok", ok);
/* -> {"event":"rx_done","len":<n>,"crc_ok":<ok>} */
```

From an ISR:

```c
RIG_EMIT_EVENT_FROM_ISR("rx_done", "len", n);
```

enqueues a fixed-shape record (event name pointer + up to a small fixed number of
scalar key/value pairs, the keys being string literals stored as pointers) into a
lock-free ring of depth `CONFIG_RIGLINK_EVENT_QUEUE_DEPTH`. `rig_run()` drains
the ring and emits the lines. On overflow the oldest record is dropped and a
`dropped` count is surfaced on the next emitted event.

### Logging passthrough

```c
rig_log("got %d packets", n);   /* -> {"log":"got 3 packets"} */
```

Gated by `CONFIG_RIGLINK_LOG`. Lets device diagnostics land in the host
transcript and on failing-test reports even when normal `LOG_INF` output goes to
a different sink or is too noisy to capture.

### Registration

Each `RIG_EXPOSE` / `RIG_FN` / `RIG_EXPOSE_VAR` emits a
`static struct rig_cmd { const char *name; ...; struct rig_cmd *next; }` together
with an `__attribute__((constructor))` function that prepends it to a global head
pointer. The parser walks the list. Consequences:

- No `RIGLINK_MAX_CMDS` ceiling.
- No init-order concern — each registration is independent.
- The `struct rig_cmd` lives in RAM (≈16–24 bytes each: name pointer, function
  pointer, arg-type descriptor pointer, `next`). For ~50 exposed commands,
  ≈1 KB RAM.
- Fallback: `rig_register(&cmd)` for the rare toolchain whose startup does not
  process `.init_array` (Zephyr's GCC builds do).

### Built-in commands

- `rig.list` — `{"cmds":[{"name":"foo","ret":"int","args":["int","int"]}, ...]}`.
  Drives host-side validation, the `rig list` CLI, and `rig stubs`.
- `rig.reset` — emits its response, flushes output, then calls `rig_reset()`.
  The host tolerates either receiving the response or the port dropping.
- `rig.echo` — round-trips its arguments; a liveness check.
- `rig.peek <addr> <len>` / `rig.poke <addr> <val>` — raw memory access, present
  only when `CONFIG_RIGLINK_MEM_ACCESS=y` (off by default: a footgun on
  MPU/MMU targets and a security risk if left enabled in production).

### Source modules

- `riglink_proto.c` — input line assembly and overflow handling; the
  whitespace/quote tokenizer; the token → C-type parsers.
- `riglink_cmd.c` — the command registry (intrusive list + constructor),
  dispatch, the built-ins, and the `RIG_EXPOSE_VAR` get/set glue.
- `riglink_io.c` — the output side: sentinel prefix + `jcon` wiring; the
  response / error / event / log emitters; the ISR-deferred-event ring.
- `riglink_core.c` — `rig_init` / `rig_run` / `rig_deinit`; the optional thread.
- `riglink.h` — the public API and the `RIG_*` macros, including the preprocessor
  machinery (arg counting, the `RIG_CTYPE_*` / `RIG_PARSE_*` / `RIG_EMIT_*`
  keyword dictionaries, the `void` probe).

### Kconfig knobs (with `#define` defaults for plain-CMake builds)

- `RIGLINK` — enable the library.
- `RIGLINK_LINE_BUF_SIZE` — max input command line length.
- `RIGLINK_MAX_ARGS` — max arguments per call.
- `RIGLINK_SCRATCH_SIZE` — scratch buffer for `str` arguments / decoding.
- `RIGLINK_EVENT_QUEUE_DEPTH` — depth of the ISR-deferred-event ring.
- `RIGLINK_SENTINEL` — override the line sentinel string.
- `RIGLINK_MINIFY` — minify emitted JSON (default y).
- `RIGLINK_THREAD` (+ `_STACK_SIZE`, `_PRIO`) — spawn a dedicated `rig_run` thread.
- `RIGLINK_LOG` — enable `rig_log`.
- `RIGLINK_MEM_ACCESS` — enable `rig.peek` / `rig.poke` (default n).

## 3. Host (Python) library

Distribution and import name `riglink`; console script `rig`; depends on
`pyserial`. The `Device` class is usable standalone — the `pytest` plugin is a
convenience layer on top of it.

### Connection and the dynamic device

```python
import riglink
dev = riglink.connect("/dev/ttyACM0", baud=115200)
```

`connect()` opens the serial port, sends `rig.list` **once**, caches the result
for the lifetime of the session, and builds a `Device` whose attributes are the
discovered commands:

```python
dev.foo(3, 4)            # validate name + arity + arg types against the cached
                         # signature, send "foo 3 4", return 0
dev.parse_id("abc-123")  # str arg → quoted token
dev.dump_state()         # void + emit → return {"uptime": ..., "mode": "..."}
                         # (or None if nothing was emitted)
dev.foo(3)               # RiglinkSignatureError, raised locally — no round trip
```

A background reader thread reads bytes from the port, splits on `\n`, and routes
each sentinel line: a response fulfils the single in-flight call's future; an
`event` line is pushed onto an `events` deque; a `log` line onto a `logs` list;
non-sentinel lines onto a `device_log` list. Calls are synchronous with a
per-call timeout (default 1 s, overridable per call and via config).

Exceptions:

- `RiglinkSignatureError` — client-side validation failure (bad name, arity, or
  argument type) before anything is sent.
- `RiglinkProtocolError` — the device returned an `error` envelope; carries
  `code` and the envelope's other fields.
- `RiglinkAssertError` — subclass of `RiglinkProtocolError` for `code == "assert"`.
- `RiglinkTimeout` — no response within the timeout.

### Events

```python
ev = dev.expect_event("rx_done", timeout=1.0, len=12)   # optional field matchers
```

Waits for the next event named `rx_done` whose fields match the given keyword
arguments; returns its dict, or raises `RiglinkTimeout`. `dev.drain_events()`
and `dev.events` are available for manual handling. (A firmware-side event ring
buffer — replaying events that fired before the host attached — is a deferred
item; the host reader queue covers the connected case.)

### Reset

`dev.reset()` sends `rig.reset`, tolerates either a response or the port
dropping, reconnects with backoff, and re-handshakes — reusing the cached
signature set, since a soft reset does not change the firmware build.
`dev.reset(refresh=True)` re-fetches `rig.list`.

### Transcript

Every send and receive is recorded. On a failing test the transcript — commands,
responses, events, logs, and raw device console output — is attached to the
`pytest` report.

### The `rig` CLI

- `rig list --port …` — pretty-print the command registry.
- `rig call --port … foo 3 4` — invoke a command and print the response.
- `rig monitor --port …` — interactive REPL: type commands, see responses,
  events, and logs live. For bring-up and manual poking.
- `rig stubs --port … -o device_api.pyi` — turn `rig.list` into a typed stub for
  editor autocomplete and `mypy`. Purely additive tooling.

### The `pytest` plugin (`pytest11` entry point)

- Options: `--riglink-port` (repeatable, for multiple devices), `--riglink-baud`,
  `--riglink-reset=session|module|function` (default `function`); plus a
  `[tool.riglink]` config table mirroring the options.
- Fixtures:
  - `riglink_session` — session-scoped; one connected `Device` per
    `--riglink-port`.
  - `dev` — function-scoped; applies the reset policy, clears the event / log /
    transcript buffers, and yields the device.
  - `riglink_devices` — for multi-device tests (list / dict keyed by port).
- `@pytest.mark.riglink(reset="function", port="...")` overrides per test.
- `pytest_runtest_makereport` attaches the transcript on failure.

## 4. Testing and CI

- **Protocol unit tests** — `riglink_proto` (tokenizer, type parsers, line
  assembly and overflow) compiled natively, in the style of `jcon/tests/`.
- **Host unit tests** — `Device` / `riglink` against a fake serial loopback (no
  hardware): handshake, call/response, error envelopes, event matching,
  reset/reconnect, transcript capture.
- **End-to-end in CI with no hardware** — build `samples/echo` for Zephyr's
  `native_sim` target (firmware compiled as a host binary, its UART on a PTY) and
  run the `pytest` plugin against it. The same suite targets real hardware via
  `--riglink-port`.
- **Samples** — `samples/echo`: a few `RIG_EXPOSE`d arithmetic functions; an
  `RIG_FN` using `rig_emit` and `RIG_ASSERT`; an `RIG_EXPOSE_VAR`; a `k_timer`
  that fires `RIG_EMIT_EVENT`. Ships with the host test file that exercises it.

## Deferred to a later version

- Firmware-side event ring buffer — replay events that fired before the host
  attached, or during a host-side stall.
- A `buf` type keyword — built-in base64 byte buffers as a first-class arg/return
  type.
- `rig.peek` / `rig.poke` ship in v1 but default off.
- `rig stubs` is in v1 but optional tooling, not on the critical path.

## Open items

- Macro prefix: `RIG_*` (chosen, mirrors the `rig` CLI) vs. `RIGLINK_*`.
