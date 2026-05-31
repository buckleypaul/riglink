# samples/echo_shell — riglink via Zephyr's shell

Same exposed surface as [`samples/echo`](../echo/), but driven by Zephyr's
**shell subsystem** instead of a custom polling loop. No `rig_putc` / `rig_getc`
implementation in the app, no `rig_run()` pump — the shell thread does line
assembly and tokenisation, and `zephyr/shell_backend.c` (compiled in by
`CONFIG_RIGLINK_BACKEND_SHELL=y`) bridges to riglink's registry.

## Why use this backend

- **RX is IRQ-driven**: bytes are buffered in the shell's ring, so the host can
  send commands back-to-back without dropping characters.
- **No CPU-burning poll loop**: dispatch happens on shell events.
- **Native command surface**: every `RIG_EXPOSE` / `RIG_FN` shows up as
  `rig <name>` under the shell — tab completion (if you enable it), history (if
  you enable it), and consistent error handling all come for free.

## Trade-offs

- **Single UART**: the shell owns the serial port. If you also want plain-text
  logs on the same wire you must either (a) disable
  `CONFIG_LOG_BACKEND_UART` (done here), (b) route logs to RTT
  (`CONFIG_LOG_BACKEND_RTT=y`), or (c) accept `CONFIG_LOG_BACKEND_SHELL=y` (logs
  get serialised against responses on the shell thread).
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

## What's not included

- The existing Python integration suite drives `samples/echo`, not this sample.
  The wire envelope is unchanged, but the request prefix is `rig ` (space) on
  this backend, so adapting `python/riglink/transport.py` or pointing the
  conftest at this sample requires a small follow-up.
