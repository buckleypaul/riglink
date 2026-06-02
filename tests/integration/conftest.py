"""Integration fixtures: build+launch samples/echo on native_sim, or attach to a
real device via --riglink-port.

The native_sim build/launch/pseudotty-scan logic lives in the installable
package (``riglink.native_sim``) so any consumer can reuse it; this conftest
only wires the shared helper into a parameterized (echo + echo_shell) session
fixture.

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

import shutil
import tempfile

import pytest

import riglink
from riglink.native_sim import NativeSim, NativeSimError, build_native_sim


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
    try:
        # Build first so a build failure skips before we launch anything.
        exe = build_native_sim(f"samples/{sample}", build_dir)
        ns = NativeSim(exe, baud, shell_root=shell_root)
    except NativeSimError as e:
        shutil.rmtree(build_dir, ignore_errors=True)
        pytest.skip(str(e))
    yield ns
    ns.teardown()
    shutil.rmtree(build_dir, ignore_errors=True)


@pytest.fixture
def dev(device):
    # `device` is a riglink.Device (real port) or a NativeSim wrapper that
    # proxies to one. NativeSim.reset() kills + relaunches the process; both
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
