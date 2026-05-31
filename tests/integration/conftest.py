"""Integration fixtures: build+launch samples/echo on native_sim, or attach to a
real device via --riglink-port.

Note on option conflict resolution:
  The riglink pytest plugin (auto-loaded via the package's pytest11 entry point)
  already registers --riglink-port (action="append", dest="riglink_port") and
  --riglink-baud.  Re-adding them here would raise a ValueError at collection
  time.  This conftest does NOT add those options; instead it reads them via
  request.config.getoption(), which resolves to the plugin-registered values.
  --riglink-port returns a list (from action="append"); we take the first
  element (or None) as the single real-device port to connect to.
"""
from __future__ import annotations

import os
import re
import select
import shutil
import subprocess
import time
import tempfile

import pytest

import riglink

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def _build_native_sim(sample: str, build_dir: str) -> str:
    west = shutil.which("west")
    if not west:
        pytest.skip("west not on PATH; cannot build native_sim sample")
    extra_modules = os.environ.get("RIGLINK_EXTRA_ZEPHYR_MODULES")
    if extra_modules is None:
        extra_modules = ";".join((
            _REPO_ROOT,
            os.path.join(_REPO_ROOT, "third_party", "jcon"),
        ))

    cmd = [
        west, "build", "-b", "native_sim", "-d", build_dir,
        os.path.join(_REPO_ROOT, "samples", sample),
    ]
    if extra_modules:
        cmd.extend(["--", f"-DEXTRA_ZEPHYR_MODULES={extra_modules}"])

    rc = subprocess.run(cmd, cwd=_REPO_ROOT, capture_output=True, text=True)
    if rc.returncode != 0:
        pytest.skip(f"native_sim build failed:\n{rc.stdout}\n{rc.stderr}")
    exe = os.path.join(build_dir, "zephyr", "zephyr.exe")
    if not os.path.exists(exe):
        pytest.skip("native_sim build produced no zephyr.exe")
    return exe


def _launch_and_get_pty(exe: str):
    proc = subprocess.Popen(
        [exe],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    pty_path = None
    lines: list[str] = []
    last_pty_at = None
    deadline = time.monotonic() + 10.0
    while time.monotonic() < deadline:
        assert proc.stdout is not None
        ready, _, _ = select.select([proc.stdout], [], [], 0.05)
        if not ready:
            if (
                pty_path is not None
                and last_pty_at is not None
                and time.monotonic() - last_pty_at > 0.25
            ):
                break
            if proc.poll() is not None:
                break
            continue
        line = proc.stdout.readline()
        if not line:
            if proc.poll() is not None:
                break
            continue
        lines.append(line)
        m = re.search(r"connected to pseudotty:\s*(\S+)", line)
        if m:
            pty_path = m.group(1)
            last_pty_at = time.monotonic()
    if pty_path is None:
        proc.terminate()
        output = "".join(lines[-20:])
        pytest.skip(f"native_sim sample did not report a pseudotty path\n{output}")
    return proc, pty_path


class _NativeSim:
    """Manages a native_sim process lifetime across test-function resets.

    The binary is built once (session scope).  After each per-test reset
    (which kills the process), :meth:`ensure_alive` relaunches the binary,
    re-parses the PTY path, and re-connects so that ``self.dev`` is always
    a live :class:`riglink.Device`.
    """

    def __init__(self, exe: str, baud: int, shell_root: str | None = None) -> None:
        self._exe = exe
        self._baud = baud
        self._shell_root = shell_root
        self._proc: subprocess.Popen | None = None
        self._dev: riglink.Device | None = None
        # Do the initial launch
        self._launch()

    def _launch(self) -> None:
        proc, pty = _launch_and_get_pty(self._exe)
        # native_sim PTYs need a brief settle before opening
        time.sleep(0.2)
        dev = riglink.connect(pty, self._baud, timeout=5.0, ready_timeout=5.0,
                              shell_root=self._shell_root)
        self._proc = proc
        self._dev = dev

    @property
    def dev(self) -> riglink.Device:
        assert self._dev is not None
        return self._dev

    def __getattr__(self, name: str):
        return getattr(self.dev, name)

    def is_alive(self) -> bool:
        if self._proc is None:
            return False
        return self._proc.poll() is None

    def reset(self) -> None:
        if self._dev is not None:
            try:
                self._dev.reset(reconnect=False)
            except Exception:
                pass  # process may exit before or while answering reset
        if self._proc is not None:
            try:
                self._proc.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                pass
        self.ensure_alive()

    def ensure_alive(self) -> None:
        """If the process has exited, close the old device and relaunch."""
        if self.is_alive():
            return
        # Close the stale device (best-effort; transport is already dead)
        if self._dev is not None:
            try:
                self._dev.close()
            except Exception:
                pass
            self._dev = None
        # Reap the old process
        if self._proc is not None:
            try:
                self._proc.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                self._proc.kill()
            self._proc = None
        self._launch()

    def teardown(self) -> None:
        """Close the device and terminate the process (session teardown)."""
        if self._dev is not None:
            try:
                self._dev.close()
            except Exception:
                pass
            self._dev = None
        if self._proc is not None:
            self._proc.terminate()
            try:
                self._proc.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                self._proc.kill()
            self._proc = None


# Each sample is built+launched once (session scope) and driven by test_echo.py.
# echo  → poll backend (shell_root=None); echo_shell → shell backend (root "rig"),
# whose subcommand routing is what shell_root teaches the host to speak.
_SAMPLES = [
    pytest.param(("echo", None), id="echo"),
    pytest.param(("echo_shell", "rig"), id="echo_shell"),
]


@pytest.fixture(scope="session", params=_SAMPLES)
def device(request):
    sample, shell_root = request.param
    # --riglink-port is registered by the plugin as action="append"; it returns
    # a list.  Take the first element as the real-device port, or None.
    ports = request.config.getoption("--riglink-port")
    real_port = ports[0] if ports else None
    # --riglink-baud is also plugin-registered; fall back to 115200.
    baud = request.config.getoption("--riglink-baud") or 115200

    if real_port:
        # A single real port speaks one backend; drive only the poll-backend
        # (echo) param against it. A shell-backend device would need its own
        # port + shell_root, which this harness doesn't wire up.
        if shell_root is not None:
            pytest.skip("real --riglink-port drives the poll-backend (echo) param only")
        dev = riglink.connect(real_port, baud)
        yield dev
        dev.close()
        return

    build_dir = tempfile.mkdtemp(prefix=f"riglink-{sample}-build-")
    exe = _build_native_sim(sample, build_dir)
    ns = _NativeSim(exe, baud, shell_root=shell_root)
    yield ns
    ns.teardown()
    shutil.rmtree(build_dir, ignore_errors=True)


@pytest.fixture
def dev(device):
    # `device` is a riglink.Device (real port) or a _NativeSim wrapper that
    # proxies to one. _NativeSim.reset() kills + relaunches the process; both
    # paths leave a live device ready for the test.
    device.reset()
    device.clear_buffers()
    return device


@pytest.fixture
def shell_dev(dev):
    """`dev`, restricted to the shell-backend (echo_shell) param. Skips on the
    poll-backend param so shell-only assertions don't also run against echo."""
    if not getattr(dev, "_shell_root", None):
        pytest.skip("shell-backend-only assertion (poll-backend param)")
    return dev
