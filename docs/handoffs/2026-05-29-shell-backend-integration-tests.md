# Handoff — functional integration coverage for the shell backend

**Date:** 2026-05-29
**Status:** OPEN — not started.
**Area:** Python host (`python/riglink/device.py`, `_wire.py`, `connect()`),
integration harness (`tests/integration/conftest.py`), CI (`.github/workflows/zephyr.yml`),
firmware shell backend (`zephyr/shell_backend.c`, `samples/echo_shell/`).

This is review finding **#7**: the new Zephyr shell backend
(`CONFIG_RIGLINK_BACKEND_SHELL`) currently has **build-only** CI coverage. We want
to drive it functionally over `native_sim`, the same way `samples/echo` (the poll
backend) is already driven — which first requires teaching the host to speak the
shell backend's command shape.

## Current repo state (read before starting)

Several things are **uncommitted** on `main` right now:

- **The shell backend itself is unstaged/new:** `zephyr/shell_backend.c`,
  `samples/echo_shell/`, plus the `CMakeLists.txt` / `zephyr/Kconfig` /
  `.github/workflows/zephyr.yml` edits that wire it in. The `zephyr.yml` edits add
  two `west build … samples/echo_shell` steps (native_sim + nrf52dk) — **build
  only, no launch**.
- **Two small review fixes are unstaged** (from the session that produced this
  handoff), both green:
  - `src/riglink_io.c` + `tests/test_io.c`: `rig_emit_double` now guards
    non-finite values (`isfinite` → `rig_emit_null`); +2 tests.
  - `python/riglink/_wire.py` + `device.py` + `cli.py`: de-duplicated the
    integer/float keyword sets into `INT_KEYWORDS` / `FLOAT_KEYWORDS` in `_wire.py`.

Suggest committing those (logical groups) before starting this work so the branch
is clean. Baselines to confirm green first:

```bash
make test                                   # C: 8 binaries, test_io = 18 tests
cd python && python -m pytest -q            # host: 40 passed
```

## Problem

`samples/echo` (poll backend) gets build **+ functional** coverage:
`tests/integration/conftest.py` builds it for `native_sim`, launches it, parses
the PTY, connects a `riglink.Device`, and runs `tests/integration/test_echo.py`.

`samples/echo_shell` (shell backend) gets **build only**. A build proves it links
against the shell subsystem; it proves nothing about the bytes on the wire — and
the shell backend replaces the *entire* I/O + dispatch frontend, so its runtime
behavior diverges from the poll path in ways only a live test catches:

1. **Different tokenizer.** Zephyr's shell does line assembly + tokenization, then
   `rig_sub_handler` rebuilds a `struct rig_tokens` and calls `rig_dispatch`
   (`zephyr/shell_backend.c:96-135`). riglink's own `rig_line_feed` / `rig_tokenize`
   — and thus its quoting, `#` comments, `rig_io_err_overflow`, `rig_io_err_syntax`
   — never run. `rig_getc()` returns `-1` always; `rig_run()` is never called.
2. **Different output channel.** Bytes go through `shell_fprintf(sh, SHELL_NORMAL,
   "%c", c)` per byte (`shell_backend.c:46-53`), not `rig_putc`. `samples/echo_shell/prj.conf`
   already sets the machine-driven-I/O knobs (`CONFIG_SHELL_PROMPT_UART=""`,
   `…VT100_COMMANDS=n`, `…ECHO_STATUS=n`, `…HELP/HISTORY/METAKEYS/TAB=n`) — but only
   a live read of the stream confirms nothing leaks into the JSON.
3. **The concurrency design is runtime-only.** `rig_io_lock` (a `K_MUTEX`,
   `shell_backend.c:39`) serializes the event-flush `k_work` item against in-flight
   command output (`shell_backend.c:129-152`) specifically so a `tick` event line
   can't splice into the middle of a response. `echo_shell` fires `tick` at 2 Hz
   from a timer ISR. Only dispatching commands *while* that timer fires exercises
   the mutex.
4. **Different event delivery + boot.** Events drain from a `k_work_delayable` on
   `CONFIG_RIGLINK_SHELL_EVENT_POLL_MS` (default 10), not `rig_run()`. The `ready`
   event is emitted in `rig_shell_init` (`SYS_INIT`, `APPLICATION` prio) while
   `active_shell == NULL`, so it lands on the global UART shell (`shell_backend.c:156-166`).

## The blocker a functional test surfaces first

**Nothing on the host knows about the shell backend.** `encode_call(name, args)`
(`_wire.py:47-50`) emits `b"<name> <args>\n"`. But the shell backend registers all
commands as **subcommands of a root `rig` command** (`SHELL_CMD_REGISTER(rig, …)`,
`shell_backend.c:137`). So Zephyr's shell receives `add 2 3`, looks for a *root*
command `add`, doesn't find it, and errors — from the shell, not riglink. The
`echo_shell` header comment "same as samples/echo so the same tests apply" is **not
true today** without a host change.

### The exact transform (verified against the firmware)

To drive the shell backend, the host must send, for a command whose registered
name is `name`:

```
rig <shell_token> <args>\n      where  shell_token = name.removeprefix("rig.")
```

i.e. prepend the root token `rig`, and strip a leading `rig.` (because
`rig_dyn_get` registers each subcommand under `strip_rig_prefix(c->name)`,
`shell_backend.c:67-70,86`). Worked examples (all verified against
`rig_sub_handler`, which does `rig_cmd_find(argv[0])` then re-prefixes `rig.` on a
miss, `shell_backend.c:102-111`):

| Registered name | Host sends | Shell subcommand | Resolves to |
|---|---|---|---|
| `add` | `rig add 2 3` | `add` | `add` |
| `scale` | `rig scale 3 2.5` | `scale` | `scale` |
| `g_counter.set` | `rig g_counter.set 42` | `g_counter.set` | `g_counter.set` |
| `rig.list` | `rig list` | `list` | `rig.list` (re-prefixed) |
| `rig.reset` | `rig reset` | `reset` | `rig.reset` (re-prefixed) |

The response's `cmd` field is the canonical name (`rig_sub_handler` sets
`t.name = c->name`), so `_interpret` / `handshake` (which reads `resp["cmds"]`) are
unaffected. The root token and the `rig.` strip are both hardcoded on the firmware
side, so the host option is effectively fixed to root `"rig"`.

## Goal

1. `riglink.connect(port, …, shell_root="rig")` (and the underlying `Device`) can
   drive a shell-backend device transparently: typed attribute calls, the
   `rig.list` handshake, events, `reset()` — all work, with `shell_root=None`
   (default) preserving today's poll-backend behavior exactly.
2. The existing `tests/integration/test_echo.py` assertions pass against
   `samples/echo_shell` too (proving "same tests apply"), plus a few shell-only
   assertions (clean stream, event/response interleaving).
3. CI runs the shell suite, not just a build.

## What to build

### 1. Host: a `shell_root` option on `Device` / `connect`

Smallest correct seam — route the typed call path through a name transform:

- Add `shell_root: str | None = None` to `Device.__init__` (`device.py:63`) and
  `connect()` (`device.py:334`); store `self._shell_root`.
- Add a private helper, e.g.:
  ```python
  def _wire_name(self, name: str) -> str:
      if not self._shell_root:
          return name
      tok = name.removeprefix(self._shell_root + ".")   # py>=3.10 per pyproject
      return f"{self._shell_root} {tok}"
  ```
- Apply it where outbound command lines are built:
  - `_send` (`device.py:170-171`) → `encode_call(self._wire_name(name), args)`.
  - `call_raw` (`device.py:173-180`) → transform `name` before joining tokens.
  - **Leave `write_line` (`device.py:216-222`) raw** — it's the REPL escape hatch;
    a `rig monitor` user types `rig add 2 3` themselves.
- `_Call` arity/type-checking (`device.py:32-46`) operates on canonical names from
  the signature cache — **do not** change it; the transform happens only at encode.
- The handshake (`handshake`, `device.py:199-210`) calls `_send("rig.list", …)`,
  which now becomes `rig list` automatically — good, that's why the option must be
  set at `connect()` time, before the handshake runs.

Optional (nice, not required for the integration suite, which constructs its own
`Device`): a `--riglink-shell-root` plugin option in `pytest_plugin.py` so the
`dev` fixture can target real shell-backend hardware.

Add host unit coverage in `python/tests/`: a `test_wire`/`test_device` case (use
`LoopbackTransport` + the `_fakedev` pattern, or just assert the encoded bytes)
proving `shell_root="rig"` maps `add`→`rig add`, `rig.list`→`rig list`,
`g_counter.set`→`rig g_counter.set`, and that `shell_root=None` is unchanged.

### 2. Integration harness: parametrize the sample

`tests/integration/conftest.py` hardcodes `samples/echo` (`_build_native_sim`,
line 43). Generalize it:

- `_build_native_sim(sample: str, build_dir)` → build
  `os.path.join(_REPO_ROOT, "samples", sample)` (keep board `native_sim`, keep the
  `RIGLINK_EXTRA_ZEPHYR_MODULES` logic at lines 34-46 untouched).
- Parametrize the session `device` fixture over `["echo", "echo_shell"]` (e.g.
  `@pytest.fixture(scope="session", params=...)`), so `test_echo.py` runs against
  both. For the `echo_shell` param, pass `shell_root="rig"` into the
  `riglink.connect(...)` call inside `_NativeSim._launch` (`conftest.py:116-122`) —
  thread the param through `_NativeSim.__init__`.
- The real-device path (`--riglink-port`, `conftest.py:191-200`) should run the
  `echo` param only (skip `echo_shell` unless a separate shell port is given).

PTY wiring is already fine for `echo_shell`: `samples/echo_shell/boards/native_sim.conf`
sets `CONFIG_UART_NATIVE_PTY=y` with no overlay, so the shell runs on a single
console PTY that `_launch_and_get_pty` (`conftest.py:57-96`) will report via
`connected to pseudotty:`. Logs are deferred with the UART backend off
(`CONFIG_LOG_MODE_DEFERRED=y`, `CONFIG_LOG_BACKEND_UART=n`), so they won't pollute
that PTY.

### 3. Shell-only assertions

Add (e.g. `tests/integration/test_echo_shell.py`, or shell-marked cases) on top of
the shared `test_echo.py` set:

- **Clean stream:** after a batch of calls, `dev.console` contains no stray
  prompt/echo/ANSI bytes and no `<malformed:…>` entries (`device.py:113-117`).
- **Interleaving (the headline — justifies `rig_io_lock`):** loop calls for ~3 s
  while `tick` fires at 2 Hz; assert every response parses and no event spliced
  into a response (zero malformed). Hardening option: temporarily raise the tick
  rate in `echo_shell/src/main.c` (or behind a build flag) to widen the race window.
- **Assert framing survives `shell_fprintf`:** `dev.check_even(7)` raises
  `RiglinkAssertError`.
- **Handshake + ready:** `connect()` succeeds and `command_names` matches the
  shared list (`test_echo.py:5-8`). Treat `ready` as best-effort — it may be
  emitted before the host opens the PTY; `connect(wait_ready=True)` already
  tolerates missing it (`device.py:344-348`).

### 4. CI: drive it, don't just build it

In `.github/workflows/zephyr.yml`, the `echo_shell` coverage should be a
launch-and-drive run, not `west build`. Cheapest: after the existing native_sim
build/run of the integration suite, run it again pointed at the shell sample (once
the conftest is parametrized, that's automatic — the suite builds+launches both).
Locally this is `make native-sim-test` (Docker; see the
`2026-05-11-native-sim-docker.md` handoff) — confirm the parametrized suite passes
there before touching CI.

## Verification

```bash
# host unit tests (the new transform)
cd python && python -m pytest -q

# integration, both samples, in Docker (any OS):
make native-sim-test                          # echo + echo_shell, all green
make native-sim-test PYTEST_ARGS="-v -k shell"
```

Expected: the full `test_echo.py` set passes for both `echo` and `echo_shell`
params, plus the shell-only cases. The interleaving test is the one that would
fail if `rig_io_lock` were removed or mis-scoped — sanity-check it actually
exercises the race (it should fail with the mutex disabled).

## Risks & gotchas

- **Submodule.** `third_party/jcon` is a git submodule; the Docker wrapper inits it,
  but a fresh local clone needs `git submodule update --init --recursive`.
- **`native_sim` reboot.** `rig.reset` → `rig_reset()` (`__weak`,
  `sys_reboot(SYS_REBOOT_COLD)`, `shell_backend.c:60-63`) exits the native_sim
  process; `_NativeSim.reset()` (`conftest.py:137-148`) already kills + relaunches.
  Confirm the relaunched shell device reconnects with `shell_root` still set.
- **Don't transform twice / don't transform `write_line`.** Only the typed path
  (`_send`, `call_raw`) gets the transform; `rig monitor` users prefix `rig `
  themselves.
- **`removeprefix` needs py≥3.9** — fine (`requires-python = ">=3.10"`).
- **Build time.** Parametrizing doubles native_sim builds in the suite; the
  session-scoped fixture builds each sample once. ccache in the Docker volume keeps
  it cheap on warm runs.
- **Tick rate vs. flakiness.** If you raise the tick rate to stress interleaving,
  keep it modest so event-queue overflow (`_dropped`) doesn't dominate and make the
  assertions noisy.

## Pointers

- Firmware backend: `zephyr/shell_backend.c` (root cmd `:137`; `strip_rig_prefix`
  `:67`; `rig_dyn_get` `:74`; `rig_sub_handler` `:96`; mutex `:39,129`; event work
  `:144`; `rig_shell_init`/ready `:156`). Kconfig: `zephyr/Kconfig:106-131`.
- Sample: `samples/echo_shell/` (`prj.conf` shell knobs; `boards/native_sim.conf`
  PTY; `src/main.c` mirrors `samples/echo`).
- Host: `python/riglink/device.py` (`Device.__init__`, `_send`, `call_raw`,
  `write_line`, `handshake`, `connect`), `python/riglink/_wire.py` (`encode_call`),
  `python/riglink/pytest_plugin.py` (optional `--riglink-shell-root`).
- Harness: `tests/integration/conftest.py`, `tests/integration/test_echo.py`.
- CI: `.github/workflows/zephyr.yml`; Docker path: `scripts/native-sim-test.sh`,
  `docs/handoffs/2026-05-11-native-sim-docker.md`.
- Architecture context: `ARCHITECTURE.md` §2.4 (pump), §2.6 (events); the shell
  backend isn't documented there yet — **add a short §2.x once this lands**.

## Estimate

~half a day: the host `shell_root` transform + unit test is ~1 hr; the conftest
parametrization + shell-only tests + getting a clean run in Docker is the bulk.
The interleaving test is where the real value (and most of the fiddling) is.
