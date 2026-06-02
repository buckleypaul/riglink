# samples/echo_shell — riglink via Zephyr's shell

Mostly the same exposed surface as [`samples/echo`](../echo/), but driven by
Zephyr's **shell subsystem** instead of a custom polling loop. No `rig_putc` /
`rig_getc` implementation in the app, no `rig_run()` pump — the shell thread does
line assembly and tokenisation, and `zephyr/shell_backend.c` (compiled in by
`CONFIG_RIGLINK_BACKEND_SHELL=y`) bridges to riglink's registry.

## Exposed commands

In addition to the shared surface (`add`, `scale`, `board_info`,
`g_counter.get/set`, `check_even`), this sample adds two commands that
deliberately stress paths the short, no-`str`, trivial commands never reach — so
CI exercises them:

| Command | Signature | Exercises |
|---------|-----------|-----------|
| `echo_str` | `(str) -> void` | A **long `str` argument** near the input-line length: RX ring-buffer pressure, the shell tokenizer, `rig_parse_str()`, and JSON-escaped `str` emission. Echoes the argument back under an `echo` field (a `str` *return type* is unsupported — `str` is argument-only — so the echo rides an emitted field, not `ret`) plus a `len` field, so the round trip is verifiable end to end. |
| `hash_str` | `(str) -> unsigned` | A **non-trivial handler** (32-bit FNV-1a) doing real work on the shell thread's stack, with a `RIG_STR_ARG_SIZE`-wide argument buffer — so undersized `CONFIG_SHELL_STACK_SIZE` surfaces as a crash here, not silently. |

These widen `RIG_STR_ARG_SIZE` (in `src/main.c`) and
`CONFIG_RIGLINK_LINE_BUF_SIZE` (in `prj.conf`) to 192/200 bytes so a long
argument fits the input line *and* the parsed-argument buffer.

## Why use this backend

- **RX is IRQ-driven**: bytes are buffered in the shell's ring, so the host can
  send commands back-to-back without dropping characters.
- **No CPU-burning poll loop**: dispatch happens on shell events.
- **Native command surface**: every `RIG_EXPOSE` / `RIG_FN` shows up as
  `rig <name>` under the shell — tab completion (if you enable it), history (if
  you enable it), and consistent error handling all come for free.

## Give riglink exclusive use of its UART

**The shell must be the only writer on the wire that carries the JSON frames.**
riglink's sentinel lets the host *recover* from unexpected console output (a
line that doesn't start with `\x1eRIG ` is filed as console output, not parsed),
but any backend that writes to the same UART **mid-line** splices bytes into a
frame and corrupts it — the host sees a malformed line and a dropped reply. Seen
on a real DK: an async (deferred) log flush interleaved with a response and tore
the frame; disabling the UART log backend fixed it. This sample's `prj.conf`
keeps everything else off the UART:

```conf
CONFIG_UART_CONSOLE=n        # no printk()/console banner on the wire
# Log backend off the UART is set per board (boards/*.conf), since whether logs
# route to this UART is board-specific:
CONFIG_LOG_BACKEND_UART=n
# Shell stays quiet — no prompt, ANSI escapes, or echo in the stream:
CONFIG_SHELL_PROMPT_UART=""
CONFIG_SHELL_VT100_COMMANDS=n
CONFIG_SHELL_ECHO_STATUS=n
```

Logging stays compiled in (`CONFIG_LOG=y`) and is harmless on the wire as long as
no *backend* targets the UART. If you want firmware-side logs during bring-up,
route them off-wire (`CONFIG_LOG_BACKEND_RTT=y`) instead of re-enabling the UART
log backend, or accept `CONFIG_LOG_BACKEND_SHELL=y` (logs are then serialised
against responses on the shell thread instead of racing them).

## Trade-offs

- **Tokeniser differences**: Zephyr's shell tokeniser handles `"..."` quoting
  and `\\` / `\"` escapes the same way riglink does, so the existing Python
  wire codec (which only emits those two escapes and rejects sub-`0x20` bytes
  before sending) round-trips unchanged. Anything outside that contract is
  off-spec for both ends and not something the protocol promised.
- **Wire format change on the *request* side**: riglink commands are registered
  as subcommands of a root `rig` shell command, with the `rig.` prefix stripped
  for shell syntax. The host must now send `rig <name> <args...>\n` (two
  tokens before the args) instead of `<name> <args...>\n`. The *response* shape
  is unchanged — `{"cmd":"rig.add","ret":5}` exactly as before — so the
  Python `Device`'s parser does not change, but `encode_call()` would need a
  prefix option to drive this sample. Zephyr's shell cannot register
  dot-containing names at the root (root commands are compile-time macros that
  use the name as a C identifier), so the prefix split is structural rather
  than cosmetic.

## Building for `native_sim`

```bash
west build -b native_sim -d build-ns samples/echo_shell
./build-ns/zephyr/zephyr.exe
```

The simulator prints `UART_0 connected to pseudotty: /dev/pts/N`. Drive it from
a terminal:

```
echo 'rig add 2 3' > /dev/pts/N
```

Expect a JSON line back on the same PTY:

```
\x1eRIG {"cmd":"rig.add","ret":5}
```

## Building for `nrf52dk/nrf52832`

```bash
west build -b nrf52dk/nrf52832 -d build-nrf samples/echo_shell
west flash
```

The J-Link VCOM appears as `/dev/ttyACM0` (Linux) or `COMx` (Windows). The UART
log backend is disabled in `boards/nrf52dk_nrf52832.conf`; if you want to keep
firmware-side logs visible during development, enable `CONFIG_LOG_BACKEND_RTT`
in your local overlay.

## Integration tests

The Python integration suite (`tests/integration/`) builds and drives **both**
samples on `native_sim`: `tests/integration/conftest.py` parametrises `device`
over `echo` (poll backend, `shell_root=None`) and `echo_shell` (shell backend,
`shell_root="rig"`). The shared assertions live in `test_echo.py`; the
shell-only ones (including the `echo_str` / `hash_str` long-string and
wrong-argcount coverage) live in `test_echo_shell.py`. Run them with
`make native-sim-test` (Docker) or, with a local Zephyr/west, `pytest
tests/integration -k echo_shell`.
