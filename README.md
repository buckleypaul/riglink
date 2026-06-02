# riglink

**Host-driven firmware testing over serial.** A firmware test harness: C macros
expose firmware functions over a serial link (output as sentinel-framed JSON via
the [`jcon`](https://github.com/buckleypaul/jcon) library), and a Python/pytest
host library drives it.

- **Firmware half:** a Zephyr C library (`riglink`) you include in your application.
  Implement three shims; annotate functions with `RIG_EXPOSE`/`RIG_FN`; call
  `rig_run()` in your main loop.
- **Python half:** `pip install riglink` — connect to any serial port, call
  firmware functions as Python methods, assert on return values and events, and
  write pytest tests with the built-in plugin.

---

## Choosing a backend (Zephyr)

riglink has two transports. **On real hardware, prefer the shell backend.**

| Backend | Kconfig | Who owns the UART / RX | Use it when |
|---------|---------|------------------------|-------------|
| **Shell (recommended)** | `CONFIG_RIGLINK_BACKEND_SHELL=y` | Zephyr's shell — IRQ-driven RX, line assembly, log multiplexing, all for free | Almost always on real hardware |
| **Poll (bare-metal fallback)** | default | *You* provide `rig_putc`/`rig_getc` and drive `rig_run()` | No shell subsystem; non-Zephyr targets; tightest footprint |

> **Trap: do not feed `rig_getc()` from `uart_poll_in()` on real hardware.**
> The obvious poll-backend shim —
> `int rig_getc(void){ unsigned char c; return uart_poll_in(u,&c)==0?(int)c:-1; }` —
> **drops RX bytes above ~9600 baud.** `uart_poll_in()` only returns whatever is
> in the UART's RX register at the instant it is called, and the pump only polls
> between other work, so at 115200 baud (a byte every ~87 us) bytes are overwritten
> before the loop comes back. The host then sees truncated command lines and times
> out. It is fine for `native_sim`, and that is the only place this repo's samples
> use it.
>
> On a board, either use the **shell backend**, or enable the library's reference
> interrupt-driven UART shim: **`CONFIG_RIGLINK_UART_IRQ_RX=y`** (poll backend).
> That option provides `rig_putc`/`rig_getc` for you — RX is interrupt-driven into
> a ring buffer that `rig_getc()` drains, so no bytes are lost. Pick the UART with
> the `zephyr,riglink-uart` chosen node (falls back to `zephyr,console`); when it
> is on, do **not** implement `rig_putc`/`rig_getc` yourself. See
> [`samples/echo`](samples/echo) and ARCHITECTURE.md §2.10.

## Firmware quickstart

This quickstart shows the **poll backend** (it is the minimal path and works on
any target). For Zephyr on hardware, see the backend table above first.

### 1. Implement the three shims

```c
#include <riglink.h>

int  rig_putc(char c)   { /* write c to your serial port; return 0 */ }
int  rig_getc(void)     { /* read one byte (non-blocking); return -1 if empty */ }
void rig_reset(void)    { /* reboot or re-initialise your target */ }
```

> On Zephyr, set `CONFIG_RIGLINK_UART_IRQ_RX=y` instead of hand-rolling these two
> shims — it ships a safe interrupt-driven implementation (see the trap above).

### 2. Expose functions

```c
/* wrap an existing function (declare + register in one step) */
static int add(int a, int b) { return a + b; }
RIG_EXPOSE(int, add, int, int);

/* define + register in one step; args arrive as arg0, arg1, ... */
RIG_FN(float, scale, int, float) { return arg0 * arg1; }

/* expose a variable (generates .get / .set commands) */
static uint32_t g_state;
RIG_EXPOSE_VAR(uint32_t, g_state);

/* use RIG_ASSERT to report a firmware assertion failure to the host */
RIG_FN(int, safe_div, int, int) { RIG_ASSERT(arg1 != 0); return arg0 / arg1; }

/* emit extra fields in a response */
RIG_FN(void, status) {
    rig_emit("uptime_ms", (int64_t)k_uptime_get());
    rig_emit("free_mem",  (int64_t)k_mem_free_get());
}

/* emit events (thread context) */
RIG_EMIT_EVENT("sensor_ready", "temp_c", 23.5f, "hum_pct", 48.0f);

/* emit events from an ISR */
static void irq_handler(void) { RIG_EMIT_EVENT_FROM_ISR("irq_fired", "pin", 7); }

/* log a message to the host */
rig_log("calibration done, k=%d", k);
```

### 3. Supported type keywords

`int` `unsigned` `int8_t` `int16_t` `int32_t` `int64_t` `uint8_t` `uint16_t`
`uint32_t` `uint64_t` `size_t` `ptrdiff_t` `intptr_t` `uintptr_t` `bool`
`float` `double` `str`
(and `void` for return type). See `include/riglink_pp.h` for the full list.

> **Note:** `ssize_t` is not supported (it is not a freestanding C type). Use
> `intptr_t` instead.

### 4. Lifecycle

```c
rig_init();
while (rig_run()) {   /* pump: read stdin, dispatch, emit — non-blocking */
    /* ... your app work ... */
}
rig_deinit();
```

`rig_run()` dispatches at most one command per call (it never blocks), so keep
the loop tight — don't sleep for long stretches between calls, or use
`CONFIG_RIGLINK_THREAD`.

Or set `CONFIG_RIGLINK_THREAD=y` to let riglink manage its own thread. When
`CONFIG_RIGLINK_THREAD=y` the dedicated thread owns the pump: the application
must **not** also call `rig_init()` / `rig_run()` itself.

### 5. Installing as a Zephyr module

Add to your project's `west.yml`:

```yaml
projects:
  - name: riglink
    url: https://github.com/buckleypaul/riglink
    revision: main
    path: modules/lib/riglink
```

Then in your `prj.conf`:

```
CONFIG_RIGLINK=y
CONFIG_JCON=y
CONFIG_JCON_ENABLE_FLOAT=y
```

---

## Wire protocol summary

| Direction | Format |
|-----------|--------|
| Host → firmware | `name arg0 arg1 ...\n` (space-separated; strings quoted) |
| Firmware → host | Each output line is either a normal log line or a **sentinel line** |
| Sentinel prefix | `\x1eRIG ` (RS + `RIG `) — never appears in log output |

Sentinel line shapes:

| Type | Example |
|------|---------|
| Response | `\x1eRIG {"cmd":"add","ret":5}` |
| Response + extra fields | `\x1eRIG {"cmd":"status","uptime_ms":1234,"ret":null}` |
| Error / assert | `\x1eRIG {"cmd":"foo","error":{"code":"assert","file":"main.c","line":42,"msg":"cond failed"}}` |
| Event | `\x1eRIG {"event":"tick","n":7}` |
| Log | `\x1eRIG {"log":"calibration done, k=3"}` |
| Command list | `\x1eRIG {"cmd":"rig.list","cmds":[...],"ret":null}` |

The return value is always under `"ret"` (`null` for a void command); any extra
top-level fields a command emits via `rig_emit()` are siblings of `cmd` and `ret`.
On the host side a call always evaluates to a `dict`: `{"ret": <value>, **extras}`.

---

## Python host quickstart

```bash
pip install riglink
```

```python
import riglink

dev = riglink.connect("/dev/ttyACM0")   # or COMx on Windows; baud=115200 default
dev.add(2, 3)                           # {"ret": 5}
dev.scale(3, 2.5)                       # {"ret": 7}
dev.g_state_set(42)                     # {"ret": None}
dev.g_state_get()                       # {"ret": 42}
ev = dev.expect_event("tick", timeout=2.0)  # {"event": "tick", "n": 7}
dev.reset()                             # calls rig_reset() on the firmware side
dev.close()
```

`connect()` sends `rig.list`, caches the command registry, and creates typed
proxy methods for every registered command.

---

## `rig` CLI

```bash
rig list --port /dev/ttyACM0                        # print the command registry
rig call --port /dev/ttyACM0 add 2 3               # call a function, print result
rig monitor --port /dev/ttyACM0                    # stream events + logs to stdout
rig stubs --port /dev/ttyACM0 -o device.pyi       # generate a typed Python stub
```

---

## pytest plugin

The `riglink` package installs a `pytest11` plugin automatically.

### CLI options

| Option | Default | Description |
|--------|---------|-------------|
| `--riglink-port PORT` | (none) | Serial port; repeatable for multiple devices |
| `--riglink-baud N` | 115200 | Baud rate |
| `--riglink-reset {session,module,function}` | `function` | When to call `dev.reset()` |
| `--riglink-sentinel TAG` | `RIG ` | Sentinel tag (the text after the `\x1e` RS byte). A leading `\x1e` is auto-prepended if you don't include one, so the default `RIG ` yields the wire prefix `\x1eRIG `. |

### Fixtures

```python
def test_add(dev):          # single-device shorthand
    assert dev.add(2, 3) == {"ret": 5}

def test_multi(riglink_devices):   # {port: Device} for multi-device tests
    pass
```

### Per-test overrides

```python
@pytest.mark.riglink(reset="session", port="/dev/ttyACM1")
def test_something(dev): ...
```

### Running against `native_sim` (no hardware)

The `tests/integration/` suite builds `samples/echo` for `native_sim`,
launches it, and connects automatically — if `west` is on PATH and a Zephyr
workspace is initialised. Without `west`, all tests are skipped.

```bash
cd python && pip install -e .
python -m pytest ../tests/integration -q
```

On macOS, Windows, or any host without a local Linux Zephyr setup, run the same
suite in Docker:

```bash
make native-sim-test
make native-sim-test PYTEST_ARGS="-v -k test_add"
```

The Docker path uses `docker.io/zephyrprojectrtos/ci:v0.29.2` by default,
creates the Zephyr workspace in a `riglink-west` Docker volume, and keeps
pip/ccache data in `riglink-pip` and `riglink-ccache`. The first run is slow
while the image, Zephyr checkout, and caches warm. The native_sim UART PTY is
created inside the container and the Python tests run in that same container,
so no host `/dev` serial device is needed for the no-hardware path.

On a Linux host with a real serial device, set `RIGLINK_DEVICE=/dev/ttyACM0`
when running `make native-sim-test`; the wrapper passes that device through to
Docker and runs the integration suite with `--riglink-port=/dev/ttyACM0`.

With a real device:

```bash
python -m pytest ../tests/integration -q --riglink-port /dev/ttyACM0
```

---

## Kconfig knobs

| Symbol | Default | Description |
|--------|---------|-------------|
| `CONFIG_RIGLINK` | n | Enable riglink (also selects `JCON`) |
| `CONFIG_RIGLINK_LINE_BUF_SIZE` | 128 | Max command line length (bytes) |
| `CONFIG_RIGLINK_MAX_ARGS` | 8 | Max arguments per command / `RIG_EXPOSE` |
| `CONFIG_RIGLINK_SCRATCH_SIZE` | 128 | Scratch buffer for string args (bytes) |
| `CONFIG_RIGLINK_EVENT_QUEUE_DEPTH` | 8 | ISR-deferred event ring depth |
| `CONFIG_RIGLINK_EXTRA_FIELDS_MAX` | 16 | Max `rig_emit()` calls buffered per `RIG_FN` body |
| `CONFIG_RIGLINK_EXTRA_FIELDS_CAP` | 512 | Byte arena for those buffered fields' name/value strings |
| `CONFIG_RIGLINK_SENTINEL_TEXT` | `RIG ` | ASCII tag after the 0x1e (RS) byte on each output line (full prefix: `\x1e` + this) |
| `CONFIG_RIGLINK_MINIFY` | y | Minify emitted JSON |
| `CONFIG_RIGLINK_LOG` | y | Enable `rig_log()` passthrough |
| `CONFIG_RIGLINK_MEM_ACCESS` | n | Enable `rig.peek` / `rig.poke` |
| `CONFIG_RIGLINK_THREAD` | n | Spawn a dedicated thread for `rig_run()` |
| `CONFIG_RIGLINK_THREAD_STACK_SIZE` | 2048 | Thread stack size |
| `CONFIG_RIGLINK_THREAD_PRIO` | 5 | Thread priority |

---

## Deviations from the design spec

The implementation intentionally deviates from
`docs/superpowers/specs/2026-05-11-riglink-design.md` in the following ways:

1. **`ssize_t` type keyword not provided.** `ssize_t` is not a freestanding C
   type and is not guaranteed on all Zephyr targets. Use `intptr_t` as a
   portable signed-size type instead.

2. **Firmware event ring is ISR-deferral only.** The spec mentions a potential
   "replay events that fired before the host attached" buffer. This is listed as
   an open item in the spec and is not implemented. The ring is only used to
   safely hand ISR-context events to the `rig_run()` pump thread.

3. **`rig.list` payload shape.** The `cmds` array is emitted as a top-level
   sibling of `cmd` and `ret`, consistent with the `rig_emit()` envelope rule:
   `{"cmd":"rig.list","cmds":[...],"ret":null}`. The spec's example shows
   `{"cmds":[...]}` alone; the actual wire output follows the standard response
   envelope.

4. **Response return-value key is `"ret"`, always a dict on the host.** The
   original spec showed a `"result"` key and `_interpret` returning a bare value
   (or `None`, or a dict) depending on the response's content. The implementation
   uses `"ret"` on the wire (`null` for void) and `_interpret` always returns a
   `dict` `{"ret": <value>, **extras}`. See
   `docs/superpowers/specs/2026-05-12-ret-return-shape.md`.

---

## References

- Design spec: `docs/superpowers/specs/2026-05-11-riglink-design.md`
- Implementation plan: `docs/superpowers/plans/2026-05-11-riglink.md`
- Sample app: `samples/echo/`
- Integration tests: `tests/integration/`
