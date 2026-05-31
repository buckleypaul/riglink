# riglink

Host-driven firmware testing over serial. **Two codebases in one repo:**

- **Firmware half** — a Zephyr/standalone C library (`include/`, `src/`). Exposes
  firmware functions over serial as sentinel-framed JSON via vendored `jcon`.
- **Host half** — a Python/pytest library (`python/riglink/`) that drives it.

A change to the wire protocol or a type keyword usually touches **both** halves.

## Commands

```bash
# Firmware (native C tests — no Zephyr needed)
make                      # build + run all tests/test_*.c
make test                 # same
make clean

# Zephyr native_sim integration tests (runs in Docker; no local Zephyr needed)
make native-sim-test
make native-sim-test PYTEST_ARGS="-v -k test_add"

# Python host
cd python
pip install -e ".[dev]"   # pyserial + pytest
python -m pytest -q
```

CI (`.github/workflows/ci.yml`) runs `make test` and the Python suite on every PR.
`.github/workflows/zephyr.yml` covers the Zephyr build.

## Layout

| Path | What |
|------|------|
| `include/riglink.h` | Public C API + user-facing `RIG_*` macros |
| `include/riglink_pp.h` | Preprocessor type-keyword dictionaries |
| `src/riglink_*.c` | core (pump), proto (input), cmd (registry), io (output) |
| `tests/*.c` | native C tests (built by the Makefile) |
| `python/riglink/` | host library (`_wire`, `transport`, `device`, `cli`, `pytest_plugin`) |
| `python/tests/`, `tests/integration/` | Python unit + native_sim integration tests |
| `third_party/jcon/` | vendored JSON emitter (**git submodule**) |
| `samples/echo/`, `samples/echo_shell/` | example firmware apps |

## Gotchas

- **jcon is a git submodule** — clone/checkout with `--recurse-submodules`.
- **Adding a scalar type keyword** means editing the *parallel* dictionaries in
  `include/riglink_pp.h` (`RIG_CTYPE_*`, `RIG_DECL_*`, `RIG_VARSET_*`, `RIG_EMIT_*`)
  plus a `rig_parse_<kw>_` wrapper. See ARCHITECTURE.md §2.8.
- **jcon does NOT escape JSON strings** — riglink does its own escaping in `riglink_io.c`.
- **`rig_run()` dispatches at most one command per call** and never blocks — keep
  the app loop tight, or use `CONFIG_RIGLINK_THREAD=y`.
- **A host call always returns a `dict`** `{"ret": <value>, **extras}` (`ret` is
  `None` for void). No content-dependent shape.
- `ssize_t` is intentionally unsupported — use `intptr_t`.

## More

- `README.md` — usage / quickstart for both halves
- `ARCHITECTURE.md` — internals (wire protocol, macros, host design, end-to-end trace)
- `docs/superpowers/specs/`, `docs/handoffs/` — design specs and session handoffs
