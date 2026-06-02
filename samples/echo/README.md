# samples/echo — riglink echo sample

A minimal Zephyr application that demonstrates the **poll backend**: it
implements the three application shims (`rig_putc`, `rig_getc`, `rig_reset`),
exposes a small surface of functions and a variable, and fires a periodic ISR
event.

> **Hardware note.** This sample's hand-rolled `rig_getc()` uses
> `uart_poll_in()`, which is fine on `native_sim` but **drops RX bytes above
> ~9600 baud on real hardware**. On a board, prefer the shell backend
> (`samples/echo_shell`), or build this sample with `-DCONFIG_RIGLINK_UART_IRQ_RX=y`
> to use the library's reference interrupt-driven shim instead — when that option
> is on, the sample's `rig_putc`/`rig_getc` are compiled out and the library
> provides safe, non-dropping versions (RX into a ring buffer from the UART ISR).
> The `zephyr,riglink-uart` chosen node selects the UART (this sample's
> `native_sim.overlay` points it at `uart1`; on a board it falls back to
> `zephyr,console`). See ARCHITECTURE.md §2.10.

## Exposed surface

| Name | Signature | Notes |
|------|-----------|-------|
| `add` | `int add(int, int)` | via `RIG_EXPOSE` |
| `scale` | `int scale(int, float)` | via `RIG_FN` |
| `board_info` | `void board_info()` | emits `board` + `uptime_ms` fields |
| `g_counter.get` / `g_counter.set` | `int32_t` | via `RIG_EXPOSE_VAR` |
| `check_even` | `int check_even(int)` | uses `RIG_ASSERT`; fails on odd input |
| `tick` event | every 500 ms | fired from timer ISR via `RIG_EMIT_EVENT_FROM_ISR` |

## Building for `native_sim`

```bash
west build -b native_sim -d build-ns samples/echo
```

Run the binary:

```bash
./build-ns/zephyr/zephyr.exe
```

The simulator prints a line like:

```
UART_1 connected to pseudotty: /dev/pts/4
```

Connect with the Python host:

```bash
rig list --port /dev/pts/4
rig call --port /dev/pts/4 add 2 3
```

Or in Python:

```python
import riglink
dev = riglink.connect("/dev/pts/4")
print(dev.add(2, 3))          # 5
print(dev.board_info())       # {"board": "native_sim", "uptime_ms": ...}
ev = dev.expect_event("tick", timeout=2.0)
```

> **Build status.** This sample has been built and linked clean for
> `nrf52dk/nrf52832` against Zephyr v4.4.0 (≈40 KB flash, ≈9 KB RAM) — the
> `riglink` Zephyr module integrates correctly (Kconfig, CMake, the
> `__attribute__((constructor))` command registration, the `jcon` dependency,
> the timer-ISR `RIG_EMIT_EVENT_FROM_ISR`).
>
> The `native_sim` build can only be done on **Linux** (Zephyr's POSIX
> architecture refuses to build on macOS/Windows), so it has been verified by
> CI (`.github/workflows/zephyr.yml` on `ubuntu-24.04`), not locally. The
> `boards/native_sim.overlay` enables `uart1` (which Zephyr v4.4.0's native_sim
> board DTS already declares as a `zephyr,native-pty-uart` node), and
> `src/main.c` uses `DT_NODELABEL(uart1)` for `RIG_UART_NODE` on native_sim. If
> a future Zephyr version drops or renames that node, the build error will be a
> "no such node/label `uart1`" message — add an explicit
> `uart1: uart { compatible = "zephyr,native-pty-uart"; status = "okay"; };`
> node in the overlay and/or update the `RIG_UART_NODE` define. See
> `docs/superpowers/plans/2026-05-11-riglink.md`, Task 10.1, for context.

## Building for `nrf52dk/nrf52832`

```bash
west build -b nrf52dk/nrf52832 -d build-nrf samples/echo
west flash
```

The J-Link VCOM appears as `/dev/ttyACM0` (Linux) or a numbered `COMx` port
(Windows). riglink shares `uart0` with the log backend; the `\x1eRIG ` sentinel
prefix keeps riglink frames distinguishable from log output.

```bash
rig list --port /dev/ttyACM0
rig call --port /dev/ttyACM0 add 7 8
```

Run the integration suite against a flashed board:

```bash
cd python && pip install -e .
python -m pytest ../tests/integration -q --riglink-port /dev/ttyACM0
```

## Running the integration tests (native_sim — no hardware needed)

```bash
cd python && pip install -e .
python -m pytest ../tests/integration -q
```

If `west` is on PATH and a Zephyr workspace is initialised, the conftest
automatically builds `samples/echo` for `native_sim`, launches the binary,
parses the PTY path, and runs the full suite.  Without `west`, all tests are
skipped with an explanatory message.
