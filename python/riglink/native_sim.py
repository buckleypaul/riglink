"""Build, launch, and connect to a Zephyr ``native_sim`` riglink sample.

This is the host-side helper any consumer needs to drive firmware on
``native_sim`` (no real hardware): it ``west build``s a sample, launches the
resulting ``zephyr.exe``, scans its stdout for the emulated UART's pseudotty
path, and connects a :class:`riglink.Device` to it. :class:`NativeSim` also
relaunches the binary after a reset (which kills the process), so the device
stays live across per-test resets.

Multi-UART boards
-----------------
A board may emulate several UARTs, each printing its own
``connected to pseudotty: /dev/pts/N`` line. riglink runs over exactly one of
them. The ``uart_index`` parameter selects which pseudotty line to use; the
default (-1) takes the *last* one, which is the convention for riglink samples
where the riglink UART is the last instance to come up.
"""
from __future__ import annotations

import os
import re
import select
import shutil
import subprocess
import time

import riglink

__all__ = ["NativeSimError", "build_native_sim", "launch_and_get_pty", "NativeSim"]

# Repo root inferred from this file: python/riglink/native_sim.py → ../../..
_REPO_ROOT = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "..")
)

_PTY_RE = re.compile(r"connected to pseudotty:\s*(\S+)")


class NativeSimError(RuntimeError):
    """Raised when a native_sim build or launch step fails.

    Carries a human-readable ``message``; callers running under pytest typically
    turn this into a ``pytest.skip`` (no west on PATH, build failure, etc.)."""


def _resolve_repo_root(repo_root: str | None) -> str:
    return os.path.abspath(repo_root) if repo_root else _REPO_ROOT


def build_native_sim(
    sample: str,
    build_dir: str,
    *,
    repo_root: str | None = None,
    extra_zephyr_modules: str | None = None,
    board: str = "native_sim",
) -> str:
    """``west build`` a riglink ``sample`` for ``board`` into ``build_dir``.

    ``sample`` may be an absolute path or a path relative to ``repo_root`` (e.g.
    ``"samples/echo"``). Returns the path to the built ``zephyr.exe``. Raises
    :class:`NativeSimError` if west is missing or the build produces no
    executable.

    ``extra_zephyr_modules`` defaults to the riglink repo plus its vendored jcon
    submodule (overridable via the ``RIGLINK_EXTRA_ZEPHYR_MODULES`` env var) so
    out-of-tree riglink builds resolve correctly."""
    root = _resolve_repo_root(repo_root)
    west = shutil.which("west")
    if not west:
        raise NativeSimError("west not on PATH; cannot build native_sim sample")

    if extra_zephyr_modules is None:
        extra_zephyr_modules = os.environ.get("RIGLINK_EXTRA_ZEPHYR_MODULES")
    if extra_zephyr_modules is None:
        extra_zephyr_modules = ";".join((
            root,
            os.path.join(root, "third_party", "jcon"),
        ))

    sample_path = sample if os.path.isabs(sample) else os.path.join(root, sample)
    cmd = [west, "build", "-b", board, "-d", build_dir, sample_path]
    if extra_zephyr_modules:
        cmd.extend(["--", f"-DEXTRA_ZEPHYR_MODULES={extra_zephyr_modules}"])

    rc = subprocess.run(cmd, cwd=root, capture_output=True, text=True)
    if rc.returncode != 0:
        raise NativeSimError(
            f"native_sim build failed:\n{rc.stdout}\n{rc.stderr}"
        )
    exe = os.path.join(build_dir, "zephyr", "zephyr.exe")
    if not os.path.exists(exe):
        raise NativeSimError("native_sim build produced no zephyr.exe")
    return exe


def launch_and_get_pty(
    exe: str,
    *,
    uart_index: int = -1,
    timeout: float = 10.0,
) -> tuple[subprocess.Popen, str]:
    """Launch ``exe`` and return ``(process, pty_path)``.

    Scans the process stdout for ``connected to pseudotty:`` lines. A multi-UART
    board prints one per emulated UART; ``uart_index`` selects which (default -1
    = the last, the riglink-UART convention). Raises :class:`NativeSimError` (and
    terminates the process) if no pseudotty path appears within ``timeout``."""
    proc = subprocess.Popen(
        [exe],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    pty_paths: list[str] = []
    lines: list[str] = []
    last_pty_at: float | None = None
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        assert proc.stdout is not None
        ready, _, _ = select.select([proc.stdout], [], [], 0.05)
        if not ready:
            # Once we've seen at least one pseudotty and the stream has gone
            # quiet, assume all UARTs have reported and stop waiting.
            if (
                pty_paths
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
        m = _PTY_RE.search(line)
        if m:
            pty_paths.append(m.group(1))
            last_pty_at = time.monotonic()
    if not pty_paths:
        proc.terminate()
        output = "".join(lines[-20:])
        raise NativeSimError(
            f"native_sim sample did not report a pseudotty path\n{output}"
        )
    return proc, pty_paths[uart_index]


class NativeSim:
    """Manages a native_sim process lifetime across test-function resets.

    The binary is launched once; after each per-test reset (which kills the
    process), :meth:`ensure_alive` relaunches it, re-parses the PTY path, and
    re-connects so that :attr:`dev` is always a live :class:`riglink.Device`.

    Construct from a pre-built executable, or use :meth:`build_and_launch` to
    build a sample and launch it in one call.
    """

    def __init__(
        self,
        exe: str,
        baud: int = 115200,
        *,
        shell_root: str | None = None,
        uart_index: int = -1,
        timeout: float = 5.0,
        ready_timeout: float = 5.0,
        settle: float = 0.2,
    ) -> None:
        self._exe = exe
        self._baud = baud
        self._shell_root = shell_root
        self._uart_index = uart_index
        self._timeout = timeout
        self._ready_timeout = ready_timeout
        self._settle = settle
        self._proc: subprocess.Popen | None = None
        self._dev: riglink.Device | None = None
        self._pty: str | None = None
        self._launch()

    @classmethod
    def build_and_launch(
        cls,
        sample: str,
        *,
        build_dir: str | None = None,
        baud: int = 115200,
        shell_root: str | None = None,
        uart_index: int = -1,
        repo_root: str | None = None,
        board: str = "native_sim",
    ) -> "NativeSim":
        """Build ``sample`` and launch it. ``build_dir`` defaults to a tempdir.

        The caller is responsible for :meth:`teardown`; the temp build dir is
        left in place (callers that created their own ``build_dir`` clean it up
        themselves)."""
        if build_dir is None:
            import tempfile
            build_dir = tempfile.mkdtemp(prefix="riglink-native-sim-")
        exe = build_native_sim(
            sample, build_dir, repo_root=repo_root, board=board
        )
        return cls(exe, baud, shell_root=shell_root, uart_index=uart_index)

    def _launch(self) -> None:
        proc, pty = launch_and_get_pty(self._exe, uart_index=self._uart_index)
        # native_sim PTYs need a brief settle before opening
        if self._settle:
            time.sleep(self._settle)
        dev = riglink.connect(
            pty, self._baud, timeout=self._timeout,
            ready_timeout=self._ready_timeout, shell_root=self._shell_root,
        )
        self._proc = proc
        self._dev = dev
        self._pty = pty

    @property
    def dev(self) -> riglink.Device:
        assert self._dev is not None
        return self._dev

    @property
    def pty(self) -> str:
        assert self._pty is not None
        return self._pty

    def __getattr__(self, name: str):
        # Proxy unknown attributes to the live device (e.g. command calls).
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
        if self._dev is not None:
            try:
                self._dev.close()
            except Exception:
                pass
            self._dev = None
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
