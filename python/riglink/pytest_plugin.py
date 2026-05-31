"""pytest plugin: a `dev` fixture connected to a riglink device, with a
configurable reset policy and the device transcript attached to failing tests.

Options:
  --riglink-port PORT   (repeatable)            device serial port(s)
  --riglink-baud N      (default 115200)
  --riglink-reset {session,module,function}     (default function)
  --riglink-sentinel S  prefix tag for the wire sentinel (default "RIG ");
                        a leading \\x1e (ASCII RS) is auto-added if absent
  --riglink-shell-root T  root command token for shell-backend devices (e.g.
                        "rig"); default None targets the poll backend
Also reads [pytest] ini keys: riglink_port (linelist), riglink_baud, riglink_reset.

Per-test override:  @pytest.mark.riglink(reset="session", port="/dev/ttyACM1")
"""
from __future__ import annotations

import pytest

import riglink


def pytest_addoption(parser):
    g = parser.getgroup("riglink")
    g.addoption("--riglink-port", action="append", default=[], dest="riglink_port",
                help="serial port of a riglink device (repeatable)")
    g.addoption("--riglink-baud", action="store", type=int, default=None, dest="riglink_baud")
    g.addoption("--riglink-reset", action="store", default=None, dest="riglink_reset",
                choices=["session", "module", "function"])
    g.addoption("--riglink-sentinel", action="store", default=None, dest="riglink_sentinel")
    g.addoption("--riglink-shell-root", action="store", default=None, dest="riglink_shell_root",
                help="root command token for a shell-backend device (e.g. 'rig')")
    parser.addini("riglink_port", type="linelist", default=[], help="riglink device port(s)")
    parser.addini("riglink_baud", default="115200", help="riglink baud rate")
    parser.addini("riglink_reset", default="function", help="riglink reset scope")
    parser.addini("riglink_sentinel", default="RIG ",
                  help="riglink wire sentinel prefix tag (leading \\x1e auto-added)")
    parser.addini("riglink_shell_root", default="",
                  help="riglink shell-backend root command token (empty = poll backend)")


def pytest_configure(config):
    config.addinivalue_line("markers", "riglink(reset=..., port=...): per-test riglink options")


def _cfg(config, opt, ini):
    val = getattr(config.option, opt, None)
    return val if val not in (None, [], "") else config.getini(ini)


def _ports(config):
    ports = _cfg(config, "riglink_port", "riglink_port")
    return list(ports)


def _baud(config):
    return int(_cfg(config, "riglink_baud", "riglink_baud"))


def _reset_scope(config):
    return _cfg(config, "riglink_reset", "riglink_reset")


def _shell_root(config):
    val = _cfg(config, "riglink_shell_root", "riglink_shell_root")
    return val or None   # "" / None ini default → poll backend


def _sentinel(config):
    s = _cfg(config, "riglink_sentinel", "riglink_sentinel")
    if isinstance(s, str):
        if not s.startswith("\x1e"):
            s = "\x1e" + s
        return s.encode()
    return s


@pytest.fixture(scope="session")
def riglink_session(request):
    """{port: Device} for the whole session. Connects each --riglink-port once."""
    ports = _ports(request.config)
    if not ports:
        pytest.skip("no --riglink-port / riglink_port configured")
    baud = _baud(request.config)
    sentinel = _sentinel(request.config)
    shell_root = _shell_root(request.config)
    devices: dict = {}
    try:
        for p in ports:
            devices[p] = riglink.connect(p, baud, sentinel=sentinel, shell_root=shell_root)
    except Exception:
        for d in devices.values():
            try:
                d.close()
            except Exception:
                pass
        raise
    if _reset_scope(request.config) == "session":
        for d in devices.values():
            d.reset()
    yield devices
    for d in devices.values():
        d.close()


def _marker_opts(request):
    m = request.node.get_closest_marker("riglink")
    return (m.kwargs if m else {})


@pytest.fixture
def riglink_devices(request, riglink_session):
    """All connected devices ({port: Device}), with the per-test reset/clear applied."""
    scope = _marker_opts(request).get("reset", _reset_scope(request.config))
    mod_state = request.session.__dict__.setdefault("_riglink_module_reset", set())
    for port, dev in riglink_session.items():
        if scope == "function":
            dev.reset()
        elif scope == "module":
            key = (port, request.module.__name__)
            if key not in mod_state:
                dev.reset()
                mod_state.add(key)
        # session scope: reset already done in riglink_session
        dev.clear_buffers()
    return dict(riglink_session)


@pytest.fixture
def dev(request, riglink_devices):
    """The single riglink device. With multiple --riglink-port, name one with
    @pytest.mark.riglink(port=...)."""
    want = _marker_opts(request).get("port")
    if want is not None:
        if want not in riglink_devices:
            pytest.fail(f"@pytest.mark.riglink(port={want!r}) not among configured ports {list(riglink_devices)}")
        return riglink_devices[want]
    if len(riglink_devices) == 1:
        return next(iter(riglink_devices.values()))
    pytest.fail(f"multiple riglink ports configured {list(riglink_devices)}; use @pytest.mark.riglink(port=...) or the riglink_devices fixture")


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    report = outcome.get_result()
    if report.when != "call" or not report.failed:
        return
    devs = []
    fa = getattr(item, "funcargs", {})
    if "dev" in fa:
        devs = [fa["dev"]]
    elif "riglink_devices" in fa:
        devs = list(fa["riglink_devices"].values())
    for d in devs:
        try:
            parts = [d.transcript]
            if d.console:
                parts.append("--- device console ---\n" + "\n".join(d.console))
            if d.logs:
                parts.append("--- rig_log ---\n" + "\n".join(d.logs))
            report.sections.append(("riglink transcript", "\n\n".join(parts)))
        except Exception:
            pass
