---
name: riglink-integration
description: Runbook for wiring the riglink firmware-testing library into a separate C / Zephyr library so its functions can be driven and asserted on from a Python/pytest host. This skill should be used when integrating riglink into a target firmware project for the first time, exposing C functions or variables over the serial link, or writing host-side pytest tests that call firmware. Triggers include "test my firmware with riglink", "expose this function over serial", "add riglink to my Zephyr app", "write a pytest that calls my C function".
---

# riglink integration

riglink turns firmware functions into remotely-callable RPCs: C macros expose
functions over a serial link as sentinel-framed JSON, and a Python/pytest host
library (`pip install riglink`) calls them as Python methods. Integrating it
into a target library means touching **both halves** — firmware annotations +
build wiring, then host tests.

## When to use this skill

Use it when the goal is to make a *separate* C/Zephyr library testable via
riglink. Not for working on riglink's own internals — that is covered by the
riglink repo's `CLAUDE.md` and `ARCHITECTURE.md`.

## Before starting: gather facts

1. Is the target a **Zephyr** app (has `prj.conf` / `west.yml`) or
   **standalone/bare-metal** C? This changes the shim and build steps.
2. Which functions/variables should be testable? List them with their exact C
   signatures — every parameter and return type must map to a supported type
   keyword (see step 3 and `references/reference.md`).
3. Is there a free UART (or other byte stream) to dedicate to the link? It must
   not be shared with `printf`/logging that the host would misread.

If any of these is unknown, ask before generating code.

## The integration runbook

Follow these steps in order. Copy the templates in `assets/` rather than
writing shims from scratch.

### Step 1 — Add riglink as a build dependency

**Zephyr (module):** add to the target project's `west.yml`:

```yaml
projects:
  - name: riglink
    url: https://github.com/buckleypaul/riglink
    revision: main
    path: modules/lib/riglink
```

Then run `west update --recurse-submodules` (riglink vendors `jcon` as a
**git submodule** — without `--recurse-submodules` the build fails to find it).
Enable it in `prj.conf` — copy `assets/prj.conf.snippet`.

**Standalone C:** add riglink's `include/` to the include path and compile
`src/riglink_*.c` into the build. No Zephyr needed.

### Step 2 — Implement the three shims

riglink needs exactly three application-provided functions. Copy
`assets/rig_shims_template.c` into the target project and fill in the body for
its serial hardware. The contract:

```c
int  rig_putc(char c);   /* write one byte to the link; return 0 on success */
int  rig_getc(void);     /* read one byte non-blocking; return -1 if none    */
void rig_reset(void);    /* reboot / re-init the target (host calls dev.reset()) */
```

`rig_getc()` **must be non-blocking** (return `-1` when no byte is ready) — a
blocking read stalls the `rig_run()` pump.

### Step 3 — Expose the functions and variables under test

Annotate in C. Pick the macro by case:

```c
/* wrap an existing function: declare + register in one step */
RIG_EXPOSE(int, add, int, int);

/* define + register together; args arrive as arg0, arg1, ... */
RIG_FN(float, scale, int, float) { return arg0 * arg1; }

/* expose a global variable -> generates .get / .set commands */
RIG_EXPOSE_VAR(uint32_t, g_state);

/* report a firmware-side precondition failure to the host */
RIG_FN(int, safe_div, int, int) { RIG_ASSERT(arg1 != 0); return arg0 / arg1; }

/* attach extra fields to a response */
RIG_FN(void, status) { rig_emit("uptime_ms", (int64_t)k_uptime_get()); }

/* fire an event the host can wait on (thread context vs. ISR context) */
RIG_EMIT_EVENT("sensor_ready", "temp_c", 23.5f);
RIG_EMIT_EVENT_FROM_ISR("irq_fired", "pin", 7);
```

**Every parameter and return type must be a supported keyword** (the fixed-width
ints, `size_t`/`ptrdiff_t`/`intptr_t`/`uintptr_t`, `bool`, `float`, `double`,
`str`, and `void` for returns) — see the full keyword → C-type → JSON table in
`references/reference.md`. Two things to know at the call site: `ssize_t` is
intentionally **not** supported (use `intptr_t`), and `float`/`double` require
`CONFIG_JCON_ENABLE_FLOAT=y`.

### Step 4 — Drive the pump in the main loop

```c
rig_init();
while (rig_run()) {   /* non-blocking; dispatches AT MOST one command per call */
    /* ... app work; keep this tight, don't sleep long ... */
}
rig_deinit();
```

A slow loop makes the link feel laggy. Alternative: set
`CONFIG_RIGLINK_THREAD=y` to let riglink own a dedicated pump thread — in that
mode the app must **not** call `rig_init()`/`rig_run()` itself.

### Step 5 — Write the host-side pytest

Copy `assets/test_template.py`. Key facts that prevent the most common
mistakes:

- Every call returns a **`dict`** of shape `{"ret": <value>, **extras}` — never
  a bare value. `ret` is `None` for void functions. Assert
  `dev.add(2, 3) == {"ret": 5}`, not `== 5`.
- `connect()` (or the `dev` fixture) sends `rig.list`, caches the registry, and
  creates a typed proxy method per command. A `.set`/`.get` pair appears for
  each exposed variable (`dev.g_state_set(42)`, `dev.g_state_get()`).
- Wait for events with `dev.expect_event("name", timeout=...)`.
- The pytest plugin ships automatically with the `riglink` pip package; pass
  the port via `--riglink-port` and use the `dev` fixture.

### Step 6 — Verify the round trip

Smallest possible loop first:

```bash
rig list --port <PORT>          # confirm the registry shows the exposed cmds
rig call --port <PORT> add 2 3  # confirm one call returns {"cmd":"add","ret":5}
```

Then run the pytest suite. If hardware-free, build the target for `native_sim`
and follow riglink's `make native-sim-test` pattern (Docker, no local Zephyr
needed) — see `references/reference.md` for the no-hardware path.

## Reference material

`references/reference.md` holds the detailed lookups — load only the section
needed (grep these headings):

- `## Type keywords` — keyword → C-type → JSON mapping table
- `## Wire protocol` — on-wire line shapes
- `## Kconfig knobs` — `CONFIG_RIGLINK_*` symbols an integrator sets
- `## rig CLI` — `list` / `call` / `monitor` / `stubs` subcommands
- `## pytest plugin` — fixtures and `--riglink-*` options
- `## Running without hardware (native_sim)` — the no-hardware path
- `## Gotchas` — the integration failure modes

## Verifying the integration is correct

A correct integration satisfies all of:
- `rig list` returns every intended command and no unintended ones.
- Each exposed function round-trips with the expected `{"ret": ...}` dict.
- `RIG_ASSERT` failures surface to the host as an error response, not a crash.
- Events fire and are observable via `expect_event`.
- The main loop stays responsive (or `CONFIG_RIGLINK_THREAD=y` is set).
