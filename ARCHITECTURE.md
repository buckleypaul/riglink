# riglink architecture

This document describes how riglink is put together: the firmware-side C library,
the wire protocol that connects the two halves, and the Python/pytest host
library. It is aimed at someone who wants to modify riglink itself (or understand
its internals) rather than just use it — for usage, see `README.md`.

```
   ┌──────────────────────────────┐                 ┌──────────────────────────────┐
   │  TARGET (firmware)           │   serial link   │  HOST (Python)               │
   │                              │                 │                              │
   │  app: RIG_EXPOSE/RIG_FN/...  │  ───requests──▶ │  Device / _Call proxies      │
   │  riglink C library           │                 │  reader thread + _wire codec │
   │  jcon (JSON emitter)         │  ◀──sentinel─── │  pytest plugin / rig CLI     │
   │  rig_putc / rig_getc shims   │      lines      │  Transport (pyserial / loop) │
   └──────────────────────────────┘                 └──────────────────────────────┘
```

The core idea: firmware functions are *registered* into a runtime command table
on the target via constructor functions emitted by the `RIG_*` macros. The host
asks the device to enumerate that table (`rig.list`), then synthesizes a typed
Python method for each entry. Calling the method serializes a one-line text
request; the device parses it, invokes the C function, and replies with one
sentinel-framed JSON line, which the host parses back into a Python value.

---

## 1. Wire protocol

Everything on the link is line-oriented (`\n`-terminated, `\r` ignored).

### Host → firmware

```
name arg0 arg1 ...\n
```

Space-separated tokens. The first token is the command name; the rest are
arguments. A token containing whitespace, a leading `#`, or a `"` is
double-quoted, with `\"` and `\\` escapes inside the quotes. A line whose first
token starts with `#` is a comment and is ignored. Numbers are plain decimal (or
`0x...` hex for integers); booleans are the literals `true`/`false`; strings are
sent verbatim (control characters are rejected host-side because they can't
survive a newline-delimited wire). `bytes` values are base64-encoded by the host
and must be decoded by a hand-written `RIG_FN` on the firmware side.

### Firmware → host

Every line the device emits is either:

* an ordinary log/console line (whatever `printf` etc. produce), or
* a **sentinel line**: the byte `0x1e` (ASCII RS, Record Separator) followed by
  a short ASCII tag (default `RIG `), followed by one minified JSON object.

So the default on-wire prefix is `\x1eRIG `. The `0x1e` byte never appears in
normal text output, which is what lets the host cleanly separate riglink traffic
from the target's own console chatter on a shared UART. The tag is configurable
(`CONFIG_RIGLINK_SENTINEL_TEXT` / `-DRIGLINK_SENTINEL` on the firmware side,
`--riglink-sentinel` / `Device(sentinel=...)` on the host side) — both ends must
agree.

Sentinel line shapes (the JSON object after the prefix):

| Kind | Discriminator | Example |
|------|---------------|---------|
| Response | has `cmd`, has `ret` | `{"cmd":"add","ret":5}` |
| Response + extra fields | `cmd`, extras, `ret` | `{"cmd":"status","uptime_ms":1234,"ret":null}` |
| Error / assert | has `cmd`, has `error` | `{"cmd":"foo","error":{"code":"assert","file":"main.c","line":42,"msg":"cond failed"}}` |
| Event | has `event` | `{"event":"tick","n":7}` |
| Log | has `log` | `{"log":"calibration done, k=3"}` |
| Command list | response whose `cmds` array carries the registry | `{"cmd":"rig.list","cmds":[...],"ret":null}` |

The return value is always under `"ret"`; a *void* return is encoded as
`"ret":null`. Extra top-level fields (from `rig_emit()` inside a `RIG_FN` body)
appear as siblings of `cmd`, between `cmd` and `ret`. On the host side, a call
always evaluates to a `dict`: `{"ret": <value>, **extras}` — see §3.3.

Error codes: `unknown_cmd`, `arg_count` (with `expected`/`got`), `bad_args`
(with `arg`/`expected` for a bad token, or `msg` for "line too long" / "syntax
error"), `arg_too_long` (a `str` arg longer than `RIG_STR_ARG_SIZE - 1`; carries
`arg`/`got`/`max`), `assert` (with `file`/`line`/`msg`), `internal` (with `msg`).

---

## 2. Firmware side (C library)

Source layout:

| File | Responsibility |
|------|----------------|
| `include/riglink.h` | Public API: shims, lifecycle, `rig_parse_*`/`rig_emit_*`/`rig_io_*`, `struct rig_cmd`, the event-ring types, and all the user-facing `RIG_*` macros. |
| `include/riglink_pp.h` | Preprocessor machinery for the macros: arg counting, the type-keyword dictionaries (`RIG_CTYPE_*`, `RIG_DECL_*`, `RIG_EMIT_*`, `RIG_VARSET_*`), and the width-checked `rig_parse_*_` inline wrappers. |
| `src/riglink_internal.h` | Private declarations: compile-time config (`RIG_LINE_BUF_SIZE` etc., from Kconfig or plain defaults), `struct rig_line`, `struct rig_tokens`, the dispatch/registry internal hooks, the IRQ-lock shim. |
| `src/riglink_core.c` | Lifecycle (`rig_init`/`rig_deinit`) and the `rig_run()` pump; the optional dedicated-thread variant. |
| `src/riglink_proto.c` | Input pipeline: line assembly (`rig_line_feed`), tokenizer (`rig_tokenize`), and token→scalar parsers (`rig_parse_i64` etc.). |
| `src/riglink_cmd.c` | The command registry (intrusive linked list), `rig_dispatch`, and the built-in commands (`rig.echo`, `rig.reset`, `rig.list`, optional `rig.peek`/`rig.poke`). |
| `src/riglink_io.c` | All output: sentinel framing, the `rig_emit_*` field emitters (with JSON escaping for strings, which jcon does *not* do), error envelopes, events, `rig_log()`, the extra-field capture buffer, and the ISR-deferred event ring. |
| `third_party/jcon/` | A vendored streaming JSON emitter (single static writer, zero heap, byte-at-a-time `putc`). riglink owns all JSON *structure* decisions; jcon just turns `jcon_add_int64("x", 5)` etc. into bytes. |

### 2.1 The three application shims

riglink does not own a transport. The application (or test harness) provides:

```c
int  rig_putc(char c);   /* write one byte; 0 = ok */
int  rig_getc(void);     /* read one byte; -1 if none available right now (non-blocking) */
void rig_reset(void);    /* device-defined reset; rig.reset invokes it (typically reboot) */
```

They are declared in `riglink.h` so the implementation gets a signature check;
`riglink_io.c` adapts `rig_putc` onto jcon's `(void *ctx, char)` putc signature.
On Zephyr these typically wrap `uart_poll_out` / `uart_poll_in` / `sys_reboot`
(see `samples/echo/src/main.c`).

### 2.2 How functions are exposed via macros

There are three entry-point macros, all of which boil down to: *define a static
`struct rig_cmd`, define a trampoline function, and register the struct from a
`__attribute__((constructor))`.*

```c
struct rig_cmd {
    const char         *name;       /* e.g. "add" */
    rig_cmd_fn          fn;          /* the trampoline */
    const char         *ret;         /* return type name, e.g. "int", "void" */
    const char *const  *argtypes;    /* nargs type-name strings, for rig.list */
    uint8_t             nargs;       /* or RIG_NARGS_VARIADIC (0xFF) */
    struct rig_cmd     *next;        /* registry link; set by rig_register */
    bool                registered;  /* idempotency flag; set by rig_register */
};
```

**`RIG_EXPOSE(R, fn, T0, T1, ...)`** — wrap an existing function `R fn(T0, T1, ...)`.
Expands (via `RIG__TRAMPOLINE`) to:

1. (if ≥1 arg) a `static const char *const rig__argt_fn[] = { "T0", "T1", ... }`
   array, built by `RIG_PP_FE(RIG__ARGT_ONE, ...)` which stringizes each type
   keyword.
2. `static void rig__tramp_fn(const char *cmd, int argc, char **argv)` — the
   trampoline (details below).
3. `static struct rig_cmd rig__cmd_fn = { "fn", rig__tramp_fn, "R", rig__argt_fn, nargs, NULL, false };`
4. `__attribute__((constructor)) static void rig__reg_fn(void) { rig_register(&rig__cmd_fn); }`

**`RIG_FN(R, name, T0, ...) { body }`** — same as `RIG_EXPOSE`, but it *also*
declares and defines `static R rig__fnbody_name(T0 arg0, T1 arg1, ...)` (the
parameter list is generated by `RIG__FN_PARAMS` → `(void)` when there are no
args, otherwise `(T0 arg0, T1 arg1, ...)`), and the trampoline calls
`rig__fnbody_name` instead of a user function. The `{ body }` you write after the
macro becomes the body of `rig__fnbody_name`, so arguments arrive as `arg0`,
`arg1`, … and you `return` a value of type `R`.

**`RIG_EXPOSE_VAR(T, var)`** — generates *two* commands, `var.get` and `var.set`,
each with its own trampoline and `struct rig_cmd`, registered by one constructor.
`var.get` takes no args and emits `var` under `"ret"`; `var.set <T value>` parses
one token of type `T`, assigns it to `var`, and returns void. `RIG_EXPOSE_VAR(str, ...)`
is a deliberate `_Static_assert` failure — a `str` setter would have to store a
pointer to a stack buffer that dangles the moment the trampoline returns.

**The trampoline** (`RIG__TRAMPOLINE_0` for ≥1 arg, `RIG__TRAMPOLINE_1` for zero;
selected by a tiny `RIG__ISEMPTYV_n` table) does, in order:

1. Arity check: `if (argc != N) { rig_io_err_argcount(...); return; }`.
2. Argument parsing: for each arg `i`, `RIG_PP__DECL_ONE(i, Ti)` expands to
   `RIG_DECL_Ti(i, cmd)`. For scalars that's `RIG_CTYPE_Ti _ai; if (!rig_parse_Ti_(argv[i], &_ai)) { rig_io_err_badarg(cmd, i, "Ti"); return; }`.
   For `str` it's a stack `char _ai[RIG_STR_ARG_SIZE]` filled by `rig_parse_str`,
   guarded by `rig_str_arg_too_long(argv[i], sizeof _ai, &got)`: a token longer
   than `RIG_STR_ARG_SIZE - 1` raises an `arg_too_long` error (carrying `arg`,
   `got`, `max`) instead of being silently truncated and dispatched.
   The `rig_parse_<kw>_` inline wrappers in `riglink_pp.h` are width-checked: e.g.
   `rig_parse_int8_t_` calls `rig_parse_i_range(t, INT8_MIN, INT8_MAX, ...)`, so
   an out-of-range token is a `bad_args` error, not a silent truncation.
3. `rig__cmd_active = cmd;` (so `RIG_ASSERT` can name the command) and
   `rig__io_extra_begin();` (arm the `rig_emit()` capture buffer — see §2.5).
4. `if (setjmp(rig__assert_jmp) == 0) { ... }` — the `setjmp` target that
   `RIG_ASSERT` longjmps to.
5. Inside the `setjmp` arm: call the callee. If `R` is `void`:
   `CALLEE(_a0,_a1,...); rig__io_extra_end(); rig_io_resp_begin(cmd); rig_io_ret_void();`
   (emits `"ret":null`).
   Otherwise: `R _r = CALLEE(...); rig__io_extra_end(); rig_io_resp_begin(cmd); RIG_EMIT_R("ret", _r);`.
   Then `rig_io_line_end()`. `RIG_PP_IS_VOID(R)` picks the branch at preprocess
   time; `RIG_EMIT_R` maps the keyword to the right `rig_emit_*` call with the
   right cast.
6. If `setjmp` returned non-zero (`RIG_ASSERT` fired), the error line was already
   emitted by `rig_io_err_assert()` and the trampoline produces nothing further.

### 2.3 Registration at startup

`RIG_*` macros emit `__attribute__((constructor))` functions. On a normal hosted
toolchain these run before `main`. On Zephyr, riglink's Kconfig `select`s
`STATIC_INIT_GNU` so Zephyr runs the `.init_array` entries at boot. The
constructor calls `rig_register(&node)`, which prepends the node to a file-scope
intrusive singly-linked list (`g_rig_cmds` in `riglink_cmd.c`). `rig_register`
is idempotent per node via the `registered` flag — important because a node
registered first (so its `next` is `NULL`) could otherwise be re-linked after
being displaced from the head, forming a cycle that would make `rig.list` /
lookup loop forever.

The built-ins (`rig.echo`, `rig.reset`, `rig.list`, and `rig.peek`/`rig.poke`
when `CONFIG_RIGLINK_MEM_ACCESS=y`) register themselves through the same
constructor mechanism in `riglink_cmd.c`.

### 2.4 The pump: `rig_run()`

`rig_init()` initializes the line buffer, marks the pump running, and emits
`{"event":"ready","version":"0.1.0"}` so a freshly-(re)booted device is
recognizable. `rig_deinit()` just clears the running flag.

`rig_run()` is non-blocking and does at most one command's worth of work per
call:

1. `rig_io_event_flush()` — drain the ISR-deferred event ring (§2.6).
2. Up to `2 * RIG_LINE_BUF_SIZE` times: `rig_getc()`; on `-1` (no data) break.
   Feed the byte to `rig_line_feed()`. When it returns `1`, a complete line is
   in `g_rig_line.buf`:
   * if it overflowed, still tokenize (to recover the command name if it fit) and
     emit `rig_io_err_overflow`;
   * else `rig_tokenize()`; on syntax error emit `rig_io_err_syntax`, otherwise
     `rig_dispatch()`.
   Then reset the line buffer and **break** — one command per `rig_run()` call.
3. Return `true` while running, `false` after `rig_deinit()`.

The byte budget bounds work per call; the `2x` covers scanning a just-completed
line plus part of the next one. Because each call dispatches at most one command
and never blocks, the application must keep its loop tight (don't `sleep` for
long stretches between calls). Alternatively, `CONFIG_RIGLINK_THREAD=y` makes
riglink spawn a dedicated thread (`K_THREAD_DEFINE`) that calls `rig_init()` once
and then `rig_run()` in a loop with a 1 ms sleep — in that configuration the
application must *not* call `rig_init()`/`rig_run()` itself.

`rig_dispatch()` is the seam between input and registry: it ignores blank/comment
lines, calls `rig_io_extra_reset()` (clears any leftover `rig_emit` capture from a
command that longjmp'd out), looks up the name with `rig_cmd_find()`, emits
`unknown_cmd` if missing, else calls `c->fn(name, argc, &argv[1])` — note `argv`
passed to the trampoline points at the first *argument*, not the command name.

### 2.5 Output, and the "extra fields" capture

`riglink_io.c` owns all device→host bytes. A line is framed by `rig_io_line_begin()`
(emit the sentinel prefix, then `jcon_start()`) … `rig_io_line_end()` (`jcon_end()`,
plus an explicit `\n` in minified mode since jcon only newlines in pretty mode).
On top of that:

* `rig_io_resp_begin(cmd)` opens a line and writes `"cmd":<cmd>`; the trampoline
  then emits exactly one `ret` field (`rig_io_ret_void()` writes `"ret":null` for
  void returns) and calls `rig_io_line_end()`.
* `rig_emit_i64/u64/bool/double/null/str` are thin jcon wrappers, except
  `rig_emit_str` does JSON escaping (`"`, `\`, control chars → `\uXXXX`, with
  `"..."` truncation if it would overflow `RIG_SCRATCH_SIZE`) because jcon
  emits strings verbatim. The public `rig_emit(name, value)` macro is a
  `_Generic` over C scalar types onto these.
* `rig_io_err_*` each emit a complete `{"cmd":...,"error":{...}}` line.
* `rig_io_event_begin/end` frame `{"event":...}` lines; `rig_log()` emits
  `{"log":"..."}` (compiled out when `CONFIG_RIGLINK_LOG` is off).

**The extra-fields problem.** A `RIG_FN` body runs *before* its response line is
opened (the trampoline needs the return value first). So if the body calls
`rig_emit("uptime_ms", ...)`, there is no open JSON object to add it to. riglink
solves this by *capturing* those calls: `rig__io_extra_begin()` (called by the
trampoline before the body) sets `g_extra_active`, so each `rig_emit_*` instead
writes a typed record into a small fixed array (`g_extra[]`, `RIG_EXTRA_FIELDS_MAX`
slots) with names and string values copied into a byte arena (`g_extra_arena`,
`RIG_EXTRA_FIELDS_CAP` bytes) — the caller's pointers may not outlive the command.
After the body returns, `rig__io_extra_end()` clears `g_extra_active`;
`rig_io_resp_begin()` then replays the buffered records into the response between
`"cmd"` and `"ret"`. On overflow (too many fields, or arena full) capture
stops and the response gets `"_emit_overflow":true` instead of a partial set.

This interleaves correctly with events/logs/asserts emitted *from inside* the
body: `rig_io_line_begin()` clears `g_extra_active` so that line's `rig_emit_*`
calls reach jcon (the transport), and `rig_io_line_end()` re-arms it
(`g_extra_active = g_extra_in_body`) so subsequent `rig_emit()` calls in the same
body resume buffering. `RIG_ASSERT` longjmps out without calling
`rig__io_extra_end()`, so `rig_dispatch()` calls `rig_io_extra_reset()` at the
top of every command to make sure a stale fragment can't leak into the next one.

### 2.6 Events, and the ISR-deferred ring

Two ways to emit an event:

* `RIG_EMIT_EVENT("name", "k0", v0, "k1", v1, ...)` — *immediate*, thread context
  only. Expands to `rig_io_event_begin(name); rig_emit(k0,v0); rig_emit(k1,v1); ...; rig_io_event_end();`
  (unrolled for up to 4 pairs). Not safe from an ISR because jcon is not reentrant.
* `RIG_EMIT_EVENT_FROM_ISR("name", "k0", v0, ...)` — *deferred*, ISR-safe. Builds
  a fixed-shape `struct rig_evt_rec` on the stack (an event-name pointer — must be
  a string literal so it outlives the flush — plus up to `RIG_EVT_MAX_PAIRS = 4`
  integer key/value pairs, keys also literals) and calls `rig_io_event_enqueue()`,
  which pushes it into a ring (`g_evt_ring[RIG_EVENT_QUEUE_DEPTH]`) under an IRQ
  lock. On overflow it drops the oldest and bumps a `dropped` counter.

`rig_run()` calls `rig_io_event_flush()` first thing each iteration: it pops
records under the IRQ lock and emits each as a normal event line in thread
context. If anything was dropped since the last flush, the next emitted event
carries `"_dropped":<n>` and the counter resets. (The IRQ-lock shim is a no-op on
non-Zephyr builds, where `RIG_EMIT_EVENT_FROM_ISR` still works for testing.)

This ring is *only* for ISR→thread hand-off; there is intentionally no "replay
events that fired before the host attached" buffer (it's listed as an open item
in the design spec but not implemented).

### 2.7 `RIG_ASSERT`

`RIG_ASSERT(cond)` is only valid inside a `RIG_FN` body. On failure it calls
`rig_io_err_assert(rig__cmd_active, __FILE__, __LINE__, #cond)` — which emits a
complete `{"cmd":...,"error":{"code":"assert",...}}` line on its own (the body
runs before the response line is opened, so there's no jcon session to corrupt) —
then `longjmp(rig__assert_jmp, 1)` back to the trampoline's `setjmp`, which skips
the `ret` emission. `rig__assert_jmp` and `rig__cmd_active` are file-scope in
`riglink_core.c`; only one command is ever in flight, so a single jmp_buf is fine.

### 2.8 Type keyword dictionaries

`riglink_pp.h` is essentially three parallel X-macro-style tables keyed on a type
keyword:

* `RIG_CTYPE_<kw>` → the C type (`RIG_CTYPE_uint16_t` → `uint16_t`, `RIG_CTYPE_str`
  → `const char *`).
* `RIG_DECL_<kw>(i, cmd)` → declare `_a<i>` and parse `argv[i]` into it (scalars
  share `RIG_DECL_SCALAR`; `str` gets a stack buffer).
* `RIG_VARSET_<kw>(var, cmd)` → the body of a generated `<var>.set` trampoline
  (parse, then assign); `RIG_VARSET_str` is a `_Static_assert(0, ...)`.
* `RIG_EMIT_<kw>(name, v)` → the right `rig_emit_*` with the right cast (all the
  signed integer widths funnel to `rig_emit_i64`, unsigned to `rig_emit_u64`,
  `float`/`double` to `rig_emit_double`).

Supported keywords: `int`, `unsigned`, `char`, `int8_t`…`int64_t`,
`uint8_t`…`uint64_t`, `size_t`, `ptrdiff_t`, `intptr_t`, `uintptr_t`, `bool`,
`float`, `double`, `str` (plus `void` as a return type). `ssize_t` is deliberately
absent (not a freestanding C type); use `intptr_t`. Adding a new scalar keyword
means adding one line to each of the dictionaries plus a `rig_parse_<kw>_` inline
wrapper.

### 2.9 Configuration

On Zephyr these come from `zephyr/Kconfig` as `CONFIG_RIGLINK_*`; on a standalone
build `riglink_internal.h` supplies plain defaults. The knobs: line buffer size,
max args, scratch size, event-ring depth, extra-fields array size and arena size,
the sentinel tag, JSON minification, `rig_log` on/off, `rig.peek`/`rig.poke`
on/off, and the dedicated-thread option (stack size / priority). The standalone
build always minifies and never compiles in mem-access.

### 2.10 Backends: the poll pump vs. the Zephyr shell

Everything above describes the **poll backend**: the application owns `rig_putc`/
`rig_getc` and drives `rig_run()` (or `CONFIG_RIGLINK_THREAD=y` does). riglink's
own line assembly, tokenizer, and pump are the frontend.

`CONFIG_RIGLINK_BACKEND_SHELL=y` (`zephyr/shell_backend.c`) swaps that whole
frontend for Zephyr's shell. With it the application implements **no** `rig_putc`/
`rig_getc` and never calls `rig_init()`/`rig_run()`; a `SYS_INIT` (`APPLICATION`
prio) brings riglink up and starts a `k_work_delayable`. The differences that
matter:

* **Routing.** Every registered command is exposed as a *subcommand* of one root
  `rig` shell command (`SHELL_DYNAMIC_CMD_CREATE` + `SHELL_CMD_REGISTER`).
  `rig_dyn_get` lists each subcommand under `strip_rig_prefix(c->name)` (drops a
  leading `rig.`), and `rig_sub_handler` looks `argv[0]` up directly, re-prefixing
  `rig.` on a miss. It then builds a `struct rig_tokens` and calls `rig_dispatch`
  — so the registry/dispatch half (§2.3, §2.4 tail) is shared, but `rig_line_feed`/
  `rig_tokenize` (and their quoting / `#` comments / overflow + syntax errors)
  never run. `rig_getc()` always returns `-1`. One user-visible consequence: an
  *unknown* command is rejected by Zephyr's shell (the subcommand doesn't match,
  so `rig_sub_handler` is never called) — the host gets a plain shell message and
  a timeout, **not** the poll backend's `{"error":{"code":"unknown_cmd"}}`
  envelope.
* **Output.** `rig_putc` writes through `shell_fprintf(active_shell, …)` one byte
  at a time, not the poll backend's transport. `active_shell` is set only for the
  duration of a dispatch; events and the boot `ready` fall back to the global UART
  shell.
* **Events.** The ISR-deferred ring (§2.6) is drained from the `k_work_delayable`
  every `CONFIG_RIGLINK_SHELL_EVENT_POLL_MS`, not from `rig_run()`. A `K_MUTEX`
  (`rig_io_lock`) is held across a whole dispatch *and* the event flush so an event
  line can't splice into the middle of a response line (the two run on different
  threads: the shell thread vs. the system workqueue).
* **Boot/ready.** `ready` is emitted from the `SYS_INIT` while `active_shell` is
  still `NULL`, so it lands on the global UART shell — it may precede the host
  opening the port.

The host counterpart is `connect(..., shell_root="rig")` (§3.3): the poll backend
needs nothing. `samples/echo_shell` mirrors `samples/echo`'s exposed surface, and
the integration suite (`tests/integration/`) drives both over `native_sim`.

---

## 3. Host side (Python library)

Package layout (`python/riglink/`):

| Module | Responsibility |
|--------|----------------|
| `__init__.py` | Public surface: `connect`, `Device` (lazily imported so `import riglink` doesn't pull in pyserial), and the exception types. |
| `_wire.py` | Pure wire codec: `encode_token` / `encode_call` (Python value → request line) and `classify` (device line → `(kind, payload)`). No I/O. |
| `transport.py` | `Transport` ABC plus `SerialTransport` (pyserial), `LoopbackTransport` (in-process, for tests/fakes). A transport is just a byte pipe with a short-timeout `read()`. |
| `device.py` | The `Device` proxy: background reader thread, synchronous call transactions, dynamic command attributes, events/logs/console buffers, `reset()`, and `connect()`. |
| `exceptions.py` | `RiglinkError` and subclasses: `RiglinkSignatureError` (client-side, before sending), `RiglinkProtocolError` (device returned an error envelope), `RiglinkAssertError` (a `RIG_ASSERT` fired), `RiglinkTimeout`. |
| `cli.py` | The `rig` command-line tool: `list`, `call`, `monitor`, `stubs`. |
| `pytest_plugin.py` | A `pytest11` entry point: the `dev` / `riglink_devices` fixtures, the `--riglink-*` options, reset-scope policy, and attaching the device transcript to failing tests. |

### 3.1 The reader thread and line classification

`Device.__init__` takes a `Transport` and immediately starts a daemon thread
running `_read_loop`: read bytes, accumulate in `self._buf`, and for every
complete `\n`-terminated line call `_wire.classify(line, sentinel)`:

* not starting with the sentinel → `("console", text)` (verbatim device chatter);
* sentinel + JSON object with `cmd` → `("response", dict)`;
* sentinel + JSON object with `event` → `("event", dict)`;
* sentinel + JSON object with `log` → `("log", str)`;
* sentinel but JSON didn't parse or had no known key → `("malformed", text)`.

Responses go onto a `queue.Queue` (`self._pending`); events go onto a `deque`
guarded by a `Condition` (`self._evcond`) that `notify_all`s waiters; logs and
console lines append to lists; everything is also appended to `self._transcript`
as `(direction, text)` pairs. An optional `on_line(kind, payload)` callback fires
for each line (used by `rig monitor`).

### 3.2 Function discovery (the `rig.list` handshake)

`connect(port_or_transport, ...)` builds a `SerialTransport` (or accepts a
`Transport` directly), constructs a `Device`, best-effort waits for a `ready`
event, then calls `Device.handshake()`:

1. Send `rig.list` (`self._send("rig.list", [], timeout)`).
2. The response is `{"cmd":"rig.list","cmds":[ {"name":..., "ret":..., "args":[...]}, ... ],"ret":null}`
   — note the per-command `"ret"` is the *return-type name string*, distinct
   from the top-level `"ret"` (the call's return value, here `null`).
   `_interpret` (see §3.3) returns `{"ret": None, "cmds": [...]}`;
   `handshake` pulls `resp["cmds"]`.
3. Build `self._signatures = {name: {name, ret, args}}`.
4. Build `self._attr_map`: for each dotted name like `rig.list`, an alias
   `rig_list` (dots → underscores) mapping to the real name — unless the alias
   collides with another command name or alias, in which case it maps to `None`
   (ambiguous, so the attribute will refuse).

`handshake()` can be called again to refresh the registry. There's a
`_handshake` alias for backwards compatibility.

So there is **no static stub at runtime** — the device is the source of truth for
what commands exist and what their argument/return types are. (`rig stubs` can
*write* a `.pyi` from `rig.list` for editor autocompletion, but that's optional
and not consulted at runtime.)

### 3.3 Dynamic command attributes and calls

`Device.__getattr__(name)`: if `name` (or its de-aliased form via `_attr_map`)
is a known command, return a `_Call(self, command_name)`; otherwise `AttributeError`.
So `dev.add`, `dev.scale`, `dev.g_counter_set`, `dev.rig_list` all work, created
on demand from the cached signatures.

Calling a `_Call(*args, timeout=None)`:

1. Look up the signature. If the declared args aren't `["..."]` (variadic),
   check arity, then type-check each argument against the keyword (`bool` ↔ Python
   `bool`; the integer keywords ↔ `int` but not `bool`; `float`/`double` ↔
   `int`/`float`; `str` ↔ `str`/`bytes`/`bytearray`). A mismatch raises
   `RiglinkSignatureError` *before anything is sent*.
2. `Device._send(name, args, timeout)` → `_txn(encode_call(name, args), ...)`.

`encode_call` (in `_wire.py`) maps each argument with `encode_token`: `bool` →
`true`/`false` (checked before `int`, since `bool` is an `int` subclass);
`int` → `str(value)`; `float` → `repr(value)` (round-trippable) and non-finite
floats are rejected; `bytes`/`bytearray` → base64; anything else → `str()`;
control characters in a string token are rejected; tokens needing it are
double-quoted with `\`/`"` escaped. Result: one `b"name tok tok\n"` line.

`_txn` is the synchronous transaction, serialized by `self._call_lock`: drain any
stale response from the queue, record + write the request, wait up to `timeout`
for a response (`RiglinkTimeout` on timeout), then `_interpret` it.

`_interpret(resp)`:

* `"error"` present → raise `RiglinkAssertError` for code `"assert"`, else
  `RiglinkProtocolError` (carrying `code`, the error dict, and `cmd`).
* otherwise, return `{"ret": resp.get("ret"), **extras}` where
  `extras = {everything except cmd and ret}`.

So a successful call **always** evaluates to a `dict`: the firmware return value
under `"ret"` (`None` for a void return) and any emitted extras as sibling keys —
`{"ret": 5}`, `{"ret": None}`, `{"ret": 5, "uptime_ms": 1234}`, etc. There's no
content-dependent shape. (This replaces the older four-shape behavior — bare
value / wrapping dict / `None` / bare extras dict — see
`docs/superpowers/specs/2026-05-12-ret-return-shape.md`.) The built-in
convenience wrappers `dev.echo()` / `dev.peek()` unwrap `["ret"]` themselves for
ergonomics; `dev.poke()` / `dev.reset()` return `None`; `Device.handshake()`
reads `resp["cmds"]` (still a sibling key).

`call_raw(name, *tokens, timeout=None)` and `write_line(text)` are escape hatches
that bypass type-checking/encoding (used by `rig call` and `rig monitor`).

**Shell-backend routing (`shell_root`).** `connect(..., shell_root="rig")` (or
`Device(shell_root=...)`) targets the Zephyr shell backend (§2.10). It only
changes the *outbound name*: `_wire_name(name)` prepends the root token and strips
a leading `rig.` — `add` → `rig add`, `g_counter.set` → `rig g_counter.set`,
`rig.list` → `rig list`. Applied in `_send` and `call_raw` (so the `rig.list`
handshake routes correctly too), but **not** in `write_line` — a `rig monitor`
user types the `rig ` prefix themselves. Type-checking and the cached signatures
still key off the canonical names; the device echoes the canonical `cmd` back, so
`_interpret`/`handshake` are unaffected. `shell_root=None` (default) is the poll
backend, unchanged.

### 3.4 Events, logs, reset

* `expect_event(name, timeout=None, **match)` — wait (via the `Condition`) for an
  event with that name whose fields match the `**match` kwargs; pop and return
  it, or `RiglinkTimeout`. `drain_events()` / `.events` / `.logs` / `.console` /
  `.transcript` expose the buffered state; `clear_buffers()` resets them.
* `reset(...)` — send `rig.reset` (swallowing timeout/error/connection failures,
  because the device may reboot before replying); if the transport died (USB-CDC
  re-enumeration) stop the reader, `reopen()` the transport, and restart the
  reader; best-effort wait for a fresh `ready` event; optionally re-handshake.
  `SerialTransport.reopen()` retries opening the port for up to 5 s while the
  device re-enumerates; `reset_device()` pulses DTR for a hardware reset.

### 3.5 The pytest plugin

Registered via the `pytest11` entry point in `pyproject.toml`. It adds
`--riglink-port` (repeatable), `--riglink-baud`, `--riglink-reset`
(`session`/`module`/`function`), `--riglink-sentinel` (a leading `\x1e` is
auto-prepended), and matching `[pytest]` ini keys. Fixtures:

* `riglink_session` (session scope) — `{port: Device}`, connecting each port once;
  skips the suite if no port is configured; resets all devices once if reset
  scope is `session`; closes everything at teardown.
* `riglink_devices` — applies the per-test reset/clear: `function` scope resets
  before every test, `module` scope resets once per test module (tracked in a set
  on the session), `session` scope already handled; then `clear_buffers()` on
  each. Returns `dict(riglink_session)`.
* `dev` — the single device, or the one named by `@pytest.mark.riglink(port=...)`;
  fails if multiple ports are configured and none is named.

`@pytest.mark.riglink(reset=..., port=...)` overrides per test. A
`pytest_runtest_makereport` hookwrapper attaches each involved device's
transcript (plus console output and `rig_log` lines) to a failing test's report.

`tests/integration/` uses this against `samples/echo` built for `native_sim`:
`conftest.py` builds and launches it (creating the PTY) and connects, but only if
`west` is on PATH and a Zephyr workspace is initialized — otherwise the tests
skip. `make native-sim-test` runs the same suite in the Zephyr CI Docker image so
no local Linux/Zephyr/serial setup is needed.

---

## 4. End-to-end: one call, start to finish

`dev.scale(3, 2.5)` against the `echo` sample (`RIG_FN(int, scale, int, float) { return (int)(arg0 * arg1); }`):

1. **Host**: `__getattr__("scale")` → `_Call`. `_Call(3, 2.5)`: signature says
   `args == ["int", "float"]`; arity ok; `3` is an `int` (not `bool`) — ok;
   `2.5` is a `float` — ok.
2. **Host**: `encode_call("scale", (3, 2.5))` → `b"scale 3 2.5\n"`. `_txn` takes
   `_call_lock`, drains stale responses, writes the line, waits on `_pending`.
3. **Wire**: `scale 3 2.5\n` travels over the UART.
4. **Firmware**: `rig_run()` → `rig_getc()` bytes → `rig_line_feed` assembles the
   line → `rig_tokenize` → `argv = ["scale","3","2.5"]`, `argc = 2` →
   `rig_dispatch` → `rig_io_extra_reset()` → `rig_cmd_find("scale")` → the
   trampoline `rig__tramp_scale("scale", 2, &argv[1])`.
5. **Firmware (trampoline)**: `argc == 2` ✓. `RIG_DECL_int(0, cmd)`:
   `rig_parse_int_("3", &_a0)` → `_a0 = 3`. `RIG_DECL_float(1, cmd)`:
   `rig_parse_float_("2.5", &_a1)` → `_a1 = 2.5f`. `rig__cmd_active = "scale"`;
   `rig__io_extra_begin()`; `setjmp` arm: `int _r = rig__fnbody_scale(3, 2.5f)` → `7`.
   `rig__io_extra_end()`; `rig_io_resp_begin("scale")` emits `\x1eRIG {"cmd":"scale"`;
   no buffered extras; `RIG_EMIT_int("ret", 7)` → `,"ret":7`;
   `rig_io_line_end()` → `}\n`.
6. **Wire**: `\x1eRIG {"cmd":"scale","ret":7}\n`.
7. **Host (reader thread)**: line → `classify` → `("response", {"cmd":"scale","ret":7})`
   → onto `_pending`.
8. **Host (`_txn`)**: gets the dict → `_interpret`: no `error`; returns
   `{"ret": 7}` (no extras).
9. `dev.scale(3, 2.5)` evaluates to `{"ret": 7}`.

---

## 5. References

- `README.md` — usage / quickstart for both halves.
- `docs/superpowers/specs/2026-05-11-riglink-design.md` — original design spec (and "Deviations" in the README).
- `docs/superpowers/plans/2026-05-11-riglink.md` — implementation plan.
- `third_party/jcon/README.md` — the JSON emitter's design choices.
- `samples/echo/` — a minimal complete firmware application.
- `tests/` (C) and `python/tests/` + `tests/integration/` (Python) — the test suites.
