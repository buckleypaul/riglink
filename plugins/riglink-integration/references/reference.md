# riglink reference

Detailed lookups for integration. Load only the section needed.

## Type keywords

Every parameter and return type in a `RIG_EXPOSE` / `RIG_FN` signature must be
one of these keywords. They map C type ⇄ JSON value:

| Keyword | C type | JSON | Notes |
|---------|--------|------|-------|
| `int` | `int` | number | |
| `unsigned` | `unsigned int` | number | |
| `char` | `char` | number | emitted as an integer, not a 1-char string |
| `int8_t` … `int64_t` | fixed-width signed | number | |
| `uint8_t` … `uint64_t` | fixed-width unsigned | number | 64-bit emitted as number |
| `size_t` | `size_t` | number | |
| `ptrdiff_t` | `ptrdiff_t` | number | |
| `intptr_t` | `intptr_t` | number | use **instead of** `ssize_t` |
| `uintptr_t` | `uintptr_t` | number | |
| `bool` | `bool` | `true`/`false` | |
| `float` | `float` | number | needs `CONFIG_JCON_ENABLE_FLOAT=y` |
| `double` | `double` | number | needs `CONFIG_JCON_ENABLE_FLOAT=y` |
| `str` | `const char *` | string | arg copied into a per-arg `RIG_STR_ARG_SIZE` stack buffer |
| `void` | — | `null` | return type only |

- **`ssize_t` is unsupported** (not a freestanding C type, not guaranteed on
  all Zephyr targets). Use `intptr_t`.
- A `str` argument is bounded by `RIG_STR_ARG_SIZE` (a compile-time `#define` in
  `riglink_pp.h`, default 64), not by `CONFIG_RIGLINK_SCRATCH_SIZE`; override it
  before including riglink if longer strings are needed.
- Adding a *new* scalar keyword is a riglink-internal change touching the
  parallel dictionaries in `include/riglink_pp.h` — out of scope for
  integrating; pick an existing keyword instead.

## Wire protocol

| Direction | Format |
|-----------|--------|
| Host → firmware | `name arg0 arg1 ...\n` (space-separated; strings quoted) |
| Firmware → host | each line is a normal log line **or** a sentinel line |
| Sentinel prefix | `\x1eRIG ` (RS byte `0x1e` + `RIG `) — never appears in plain log output |

Sentinel line shapes:

| Type | Example |
|------|---------|
| Response | `\x1eRIG {"cmd":"add","ret":5}` |
| Response + extra fields | `\x1eRIG {"cmd":"status","uptime_ms":1234,"ret":null}` |
| Error / assert | `\x1eRIG {"cmd":"foo","error":{"code":"assert","file":"main.c","line":42,"msg":"cond failed"}}` |
| Event | `\x1eRIG {"event":"tick","n":7}` |
| Log | `\x1eRIG {"log":"calibration done, k=3"}` |
| Command list | `\x1eRIG {"cmd":"rig.list","cmds":[...],"ret":null}` |

The return value is always under `"ret"` (`null` for void). Extra fields from
`rig_emit()` are siblings of `cmd`/`ret`. On the host, a call **always**
evaluates to a `dict`: `{"ret": <value>, **extras}` — there is no
content-dependent shape.

## Kconfig knobs (Zephyr)

| Symbol | Default | Description |
|--------|---------|-------------|
| `CONFIG_RIGLINK` | n | Enable riglink (also selects `JCON`) |
| `CONFIG_RIGLINK_LINE_BUF_SIZE` | 128 | Max command line length (bytes) |
| `CONFIG_RIGLINK_MAX_ARGS` | 8 | Max args per command / `RIG_EXPOSE` |
| `CONFIG_RIGLINK_SCRATCH_SIZE` | 128 | Scratch buffer for string args (bytes) |
| `CONFIG_RIGLINK_EVENT_QUEUE_DEPTH` | 8 | ISR-deferred event ring depth |
| `CONFIG_RIGLINK_EXTRA_FIELDS_MAX` | 16 | Max `rig_emit()` calls buffered per `RIG_FN` body |
| `CONFIG_RIGLINK_EXTRA_FIELDS_CAP` | 512 | Byte arena for those fields' name/value strings |
| `CONFIG_RIGLINK_SENTINEL_TEXT` | `RIG ` | ASCII tag after the `0x1e` byte (full prefix: `\x1e` + this) |
| `CONFIG_RIGLINK_MINIFY` | y | Minify emitted JSON |
| `CONFIG_RIGLINK_LOG` | y | Enable `rig_log()` passthrough |
| `CONFIG_RIGLINK_MEM_ACCESS` | n | Enable `rig.peek` / `rig.poke` |
| `CONFIG_RIGLINK_THREAD` | n | Spawn a dedicated thread for `rig_run()` |
| `CONFIG_RIGLINK_THREAD_STACK_SIZE` | 2048 | Thread stack size |
| `CONFIG_RIGLINK_THREAD_PRIO` | 5 | Thread priority |

`CONFIG_JCON=y` and `CONFIG_JCON_ENABLE_FLOAT=y` (for float/double) are also
required. `CONFIG_SERIAL=y` and `CONFIG_REBOOT=y` are needed by the typical
UART shim implementation.

## `rig` CLI

```bash
rig list    --port /dev/ttyACM0              # print the command registry
rig call    --port /dev/ttyACM0 add 2 3      # call a function, print result
rig monitor --port /dev/ttyACM0              # stream events + logs to stdout
rig stubs   --port /dev/ttyACM0 -o device.pyi  # generate a typed Python stub
```

Generating a `.pyi` stub gives editors/agents autocomplete and types for the
exposed surface — useful right after the registry stabilises.

## pytest plugin

Installed automatically with the `riglink` pip package (`pytest11` entry point).

CLI options:

| Option | Default | Description |
|--------|---------|-------------|
| `--riglink-port PORT` | (none) | Serial port; repeatable for multiple devices |
| `--riglink-baud N` | 115200 | Baud rate |
| `--riglink-reset {session,module,function}` | `function` | When to call `dev.reset()` |
| `--riglink-sentinel TAG` | `RIG ` | Sentinel tag; a leading `\x1e` is auto-prepended |

Fixtures: `dev` (single-device shorthand) and `riglink_devices` (`{port: Device}`
for multi-device). Per-test override:
`@pytest.mark.riglink(reset="session", port="/dev/ttyACM1")`.

## Running without hardware (native_sim)

The target can be built for Zephyr's `native_sim` board and driven over a PTY —
no physical device. riglink's own repo provides the pattern via
`make native-sim-test` (runs in Docker; no local Zephyr/west install needed):

```bash
make native-sim-test
make native-sim-test PYTEST_ARGS="-v -k test_add"
```

For the target project, mirror `samples/echo/` from the riglink repo: it adds a
`boards/native_sim.overlay` mapping a second UART (e.g. `uart1`) to a PTY, sets
`CONFIG_BOARD_NATIVE_SIM` handling in the shim, and connects the host test to
that PTY. With a real device instead, pass `--riglink-port /dev/ttyACM0`.

## Gotchas

- **`jcon` is a git submodule.** Clone/checkout/`west update` with
  `--recurse-submodules` or the build can't find the JSON emitter.
- **`rig_getc()` must be non-blocking** — return `-1` when no byte is ready, or
  the pump stalls.
- **`rig_run()` dispatches at most one command per call and never blocks.** Keep
  the app loop tight, or use `CONFIG_RIGLINK_THREAD=y`.
- **With `CONFIG_RIGLINK_THREAD=y`, do not also call `rig_init()`/`rig_run()`**
  from the app — the dedicated thread owns the pump.
- **The link UART must be dedicated.** If app `printf`/logging shares it, raw
  text interleaves with sentinel lines; plain log lines are tolerated, but
  partial/garbled writes corrupt frames.
- **Assert against the returned dict, not a bare value** — every call yields
  `{"ret": <value>, **extras}` (see Wire protocol), never the unwrapped value.
- **jcon does not escape JSON strings** — riglink does its own escaping; nothing
  for the integrator to do, but don't double-escape in `str` arguments.
