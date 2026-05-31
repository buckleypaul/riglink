# Handoff — `native_sim` integration tests in Docker

*Status: implemented. Date: 2026-05-11.*

Implementation note: the checked-in wrapper uses
`docker.io/zephyrprojectrtos/ci:v0.29.2` rather than the earlier `ci-base`
candidate. `ci-base` intentionally omits the Zephyr SDK, and the v0.28.x `ci`
images carry SDK 0.17.4, which is too old for this repo's Zephyr v4.4.0 pin.

## Problem

`tests/integration/` builds `samples/echo` for Zephyr `native_sim` (Zephyr's
POSIX architecture), launches the resulting binary, and runs the host suite
against its PTY UART. That works on Linux CI today (`.github/workflows/zephyr.yml`,
`ubuntu-24.04`) but **not on macOS or Windows** — Zephyr's POSIX arch refuses to
configure on either:

```
CMake Error at zephyr/arch/posix/CMakeLists.txt:4 (message):
  The POSIX architecture only works on Linux.
```

We want a fast local feedback loop on macOS / Windows / WSL hosts: edit
firmware, run integration tests, iterate. The cleanest path is to run the
build + tests inside a Linux container — the same image CI uses — and surface
it as a single `make` target.

## Goal

`make native-sim-test` from the repo root (on any OS with Docker) builds
`samples/echo` for `native_sim` inside a Linux container and runs
`tests/integration` against it. The host machine needs only Docker. The
existing `make test` and `python -m pytest -q` flows are unaffected.

## Approach

Use the upstream **Zephyr CI Docker image** (`docker.io/zephyrprojectrtos/ci`) —
it ships with the Zephyr SDK (incl. the host toolchain for native_sim), `west`,
`cmake`/`ninja`/`dtc`, and the Python deps Zephyr's build expects. Mount the
riglink repo read-write, install riglink's host package, run the suite. No
custom Dockerfile is required for the happy path; a thin wrapper script is
enough. A `Dockerfile` becomes worthwhile only if we need to bake in extra
tooling — kept as a follow-up.

On Apple Silicon the CI image is `linux/amd64`; pass `--platform linux/amd64`
to `docker run`. Emulation makes the build ~3× slower but the suite still
finishes in well under a minute once SDK/pip caches warm.

## What to build

### 1. `scripts/native-sim-test.sh` — the entry point

A bash script the `Makefile` calls. Resolves the repo root, picks the image
tag, sets up the named volumes, runs the container, executes the build + tests.

Behavior:

- Use a pinned image tag: `ZEPHYR_DOCKER_IMAGE=${ZEPHYR_DOCKER_IMAGE:-docker.io/zephyrprojectrtos/ci:v0.29.2}`
  (current verified tag for Zephyr v4.4.0; bump intentionally).
- Detect Apple Silicon (`uname -m` is `arm64`) and set `PLATFORM_FLAG=--platform=linux/amd64`; otherwise empty.
- Mount the repo at `/workdir/modules/lib/riglink` (rw).
- Mount three named volumes for caching:
  - `riglink-pip` → `/root/.cache/pip` (pip cache survives runs)
  - `riglink-ccache` → `/root/.cache/ccache` (Zephyr's ccache; ~10× faster rebuilds)
  - `riglink-west` → `/workdir` (west workspace + Zephyr checkout)
- Forward `--riglink-port` etc. by passing all remaining CLI args through to `pytest`.
- Run a here-doc bash inside the container that:
  1. `west init -l /workdir/modules/lib/riglink` if the west workspace volume is fresh.
  2. `west update zephyr` to fetch the Zephyr tree pinned by `west.yml`.
  3. `cd /workdir/modules/lib/riglink && git submodule update --init --recursive third_party/jcon`
     (idempotent; covers the case where the host clone is fresh and submodules aren't initialized).
  4. `python3 -m pip install --quiet -e ./python[dev]` (the integration suite imports `riglink`).
  5. `python3 -m pytest tests/integration "$@"` — relies on the conftest change below
     to drive `west build` correctly inside the container.

The script's exit code is `pytest`'s. Print a one-line summary at the top
(image tag + platform flag + arg pass-through) so the developer can see what
ran without scrolling.

Acceptance: `./scripts/native-sim-test.sh -v` runs the full integration suite
inside the container and exits 0 when all tests pass.

### 2. `Makefile` target — `native-sim-test`

Add to the root `Makefile`:

```make
.PHONY: native-sim-test
native-sim-test:
	@./scripts/native-sim-test.sh
```

Keep it on its own line, no dependency on `test` (the developer runs them
separately). Don't add it to `all:` — Docker startup is too slow for the
default target.

### 3. `tests/integration/conftest.py` — `EXTRA_ZEPHYR_MODULES` auto-detect

Inside the CI image, `ZEPHYR_BASE` is preset to the image's Zephyr checkout
and there is no west workspace containing `riglink`/`jcon`. The current
conftest invokes `west build -b native_sim -d <tmp> samples/echo` from the
repo root — west can't find `riglink` or `jcon` as modules and the build
fails. Fix the conftest so it threads the module paths through:

In `tests/integration/conftest.py`, in `_build_native_sim(build_dir)` (the
function that shells out to `west`), build the cmake-extra-args list with
this logic:

```python
extra_modules = os.environ.get("RIGLINK_EXTRA_ZEPHYR_MODULES")
if not extra_modules:
    # Default: this repo + its vendored jcon submodule. Lets the suite run
    # outside a west workspace (e.g. inside the Zephyr CI Docker image) where
    # $ZEPHYR_BASE points at the image's Zephyr and modules must be supplied
    # explicitly. Inside a real west workspace, the env var should be empty
    # (don't shadow the manifest).
    jcon = os.path.join(_REPO_ROOT, "third_party", "jcon")
    extra_modules = f"{_REPO_ROOT};{jcon}"

cmd = [west, "build", "-b", "native_sim", "-d", build_dir,
       os.path.join(_REPO_ROOT, "samples", "echo"),
       "--", f"-DEXTRA_ZEPHYR_MODULES={extra_modules}"]
```

(Keep the `cwd=_REPO_ROOT` argument to `subprocess.run`; that's still right.)

Acceptance: with this change, the conftest works in three environments —
(a) a host with a riglink-aware west workspace (the env var override lets the
developer point at workspace-resolved module paths), (b) the Docker container
(the default kicks in, `ZEPHYR_BASE` from the image + the repo's submodule
populate it), (c) macOS hosts without west (the early `shutil.which("west")`
check still skips the suite).

### 4. README — point developers at it

In `README.md`, in or just after the existing "Running integration tests on
`native_sim`" section, add:

```markdown
### From macOS / Windows (or any non-Linux dev host)

`native_sim` is Linux-only (Zephyr's POSIX architecture refuses to build
elsewhere). Use the Docker target — it runs the same build CI does:

    make native-sim-test           # uses docker.io/zephyrprojectrtos/ci:v0.29.2
    make native-sim-test PYTEST_ARGS="-v -k test_add"
```

Mention the cache volumes (`riglink-pip`, `riglink-ccache`, `riglink-west`)
and that the first run is slow while they warm.

### 5. (Optional, do later) `Dockerfile`

Only if the upstream `ci` image proves insufficient — e.g. we need a specific
`pyserial` patch, or extra debug tools. A two-line `Dockerfile` (`FROM ...`,
`RUN pip install ...`) suffices when that day comes. Don't add it pre-emptively.

## Verification

After the four changes above are in place, on a macOS host:

```
docker pull docker.io/zephyrprojectrtos/ci:v0.29.2   # one-time
make native-sim-test
```

Expected (first run, cold caches, M-series Mac under amd64 emulation): ~3-5
minutes; the integration suite reports all tests passing (the suite currently
has ~9 tests: handshake, `add`, `scale`, `board_info`, the exposed variable,
`check_even` (assert pass/fail), the periodic `tick` event, `reset_clears_state`,
and the unknown-command path).

Subsequent runs with warm caches: ~30-60 seconds.

To run against a real device from inside the container on a Linux host (e.g. an
nRF52 DK on `/dev/ttyACM0`), pass it through with
`RIGLINK_DEVICE=/dev/ttyACM0`. The script adds `--device=$RIGLINK_DEVICE` to
the `docker run` args and `--riglink-port=$RIGLINK_DEVICE` to the inner pytest
args.

## Risks and gotchas

- **Image tag drift.** Pin a specific `ci` tag in the script; bump
  intentionally. An untagged `:latest` will eventually break something subtly.
- **Apple Silicon emulation.** `linux/amd64` under Rosetta works for the C
  build and the Python suite. If a future Zephyr release ships a `linux/arm64`
  CI image, switch to it (no emulation = ~3× faster).
- **`EXTRA_ZEPHYR_MODULES` separator.** The default cmake list separator is
  `;`. Inside a shell, the surrounding quotes matter; the Python `subprocess`
  call above passes the cmake arg as a single argv element, so the separator
  is not at risk there. Be careful if anyone refactors the conftest to use
  `shell=True`.
- **`west update zephyr` is run in a Docker volume.** The image supplies the
  SDK and host tools, while Zephyr source comes from this repo's `west.yml` pin.
  A cold volume has to fetch Zephyr once; warm runs reuse the checkout.
- **The `_NativeSim` fixture relaunches the binary after every `reset()` —**
  this already works inside Linux containers; verify it still works (the PTY
  path changes on each launch and the fixture re-parses it).
- **Don't ship the `riglink-*` named volumes in `git`.** They're host-side
  Docker objects; nothing to commit.

## Files this handoff touches

```
scripts/native-sim-test.sh          (new, ~50 lines)
Makefile                            (one target appended)
tests/integration/conftest.py       (`_build_native_sim` updated as above)
README.md                           (one section appended)
docs/handoffs/2026-05-11-native-sim-docker.md   (this file)
```

## When to do it

Whenever `tests/integration` starts mattering for local development — i.e.
when someone first wants to TDD a feature that needs `native_sim` and isn't
on Linux. Until then, CI is doing the job. Estimated work: a couple of hours
including verification.
