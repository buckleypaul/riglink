import textwrap


# Preamble inserted into every pytester conftest to make tests._fakedev importable.
# The sub-pytest runs in a temp dir; riglink is installed editable so its __file__
# points into the real python/ tree — we climb up to python/ and add it to sys.path.
_FAKEDEV_PREAMBLE = textwrap.dedent("""
    import sys, pathlib, riglink
    _python_dir = pathlib.Path(riglink.__file__).parent.parent
    if str(_python_dir) not in sys.path:
        sys.path.insert(0, str(_python_dir))
""")

# Session-scoped patch: riglink_session is session-scoped so it runs before any
# function-scoped monkeypatch. We patch connect directly at session scope and
# restore it at teardown.
_SESSION_PATCH = textwrap.dedent("""
    import riglink, riglink.pytest_plugin as _pp

    @pytest.fixture(scope="session", autouse=True)
    def _patch_connect():
        from tests._fakedev import FakeDevice
        fd = FakeDevice()
        fd.command("add2", "int", ["int","int"])(lambda a,b: int(a)+int(b))
        def fake(port, baud=115200, **kw):
            d = riglink.Device(fd.transport); d._handshake(); return d
        _orig = riglink.connect
        _orig_pp = _pp.riglink.connect
        riglink.connect = fake
        _pp.riglink.connect = fake
        yield
        riglink.connect = _orig
        _pp.riglink.connect = _orig_pp
""")


def test_dev_fixture_calls_device(pytester):
    pytester.makeconftest(
        "import pytest, riglink\n"
        + _FAKEDEV_PREAMBLE
        + _SESSION_PATCH
    )
    pytester.makepyfile(test_x="""
        def test_add(dev):
            assert dev.add2(2, 3) == {"ret": 5}
    """)
    result = pytester.runpytest("--riglink-port", "fake0", "-q")
    result.assert_outcomes(passed=1)


def test_transcript_attached_on_failure(pytester):
    pytester.makeconftest(
        "import pytest, riglink\n"
        + _FAKEDEV_PREAMBLE
        + _SESSION_PATCH
    )
    pytester.makepyfile(test_x="""
        def test_fails(dev):
            assert dev.add2(2, 3) == 999
    """)
    result = pytester.runpytest("--riglink-port", "fake0", "-q")
    result.assert_outcomes(failed=1)
    result.stdout.fnmatch_lines(["*riglink transcript*", "*send: add2 2 3*"])


def test_skips_without_port(pytester):
    pytester.makepyfile(test_x="def test_x(dev): pass")
    result = pytester.runpytest("-q")
    result.assert_outcomes(skipped=1)


# A connect patch that stashes the shell_root kwarg it received onto the device,
# so a test can assert the plugin threaded --riglink-shell-root through.
_SESSION_PATCH_CAPTURE_SHELL_ROOT = textwrap.dedent("""
    import riglink, riglink.pytest_plugin as _pp

    @pytest.fixture(scope="session", autouse=True)
    def _patch_connect():
        from tests._fakedev import FakeDevice
        fd = FakeDevice()
        def fake(port, baud=115200, **kw):
            d = riglink.Device(fd.transport); d._handshake()
            d.captured_shell_root = kw.get("shell_root")
            return d
        _orig = riglink.connect
        _orig_pp = _pp.riglink.connect
        riglink.connect = fake
        _pp.riglink.connect = fake
        yield
        riglink.connect = _orig
        _pp.riglink.connect = _orig_pp
""")


def test_shell_root_option_passed_to_connect(pytester):
    pytester.makeconftest(
        "import pytest, riglink\n"
        + _FAKEDEV_PREAMBLE
        + _SESSION_PATCH_CAPTURE_SHELL_ROOT
    )
    pytester.makepyfile(test_x="""
        def test_shell_root(dev):
            assert dev.captured_shell_root == "rig"
    """)
    result = pytester.runpytest("--riglink-port", "fake0", "--riglink-shell-root", "rig", "-q")
    result.assert_outcomes(passed=1)


def test_shell_root_defaults_to_none(pytester):
    pytester.makeconftest(
        "import pytest, riglink\n"
        + _FAKEDEV_PREAMBLE
        + _SESSION_PATCH_CAPTURE_SHELL_ROOT
    )
    pytester.makepyfile(test_x="""
        def test_shell_root(dev):
            assert dev.captured_shell_root is None
    """)
    result = pytester.runpytest("--riglink-port", "fake0", "-q")
    result.assert_outcomes(passed=1)


# --- App-level shell_root default (no --riglink-shell-root flag/ini) ----------
def test_shell_root_app_level_default_fixture(pytester):
    """A consumer overriding riglink_default_shell_root pins the shell backend
    without passing --riglink-shell-root every invocation."""
    pytester.makeconftest(
        "import pytest, riglink\n"
        + _FAKEDEV_PREAMBLE
        + _SESSION_PATCH_CAPTURE_SHELL_ROOT
        + textwrap.dedent("""
            @pytest.fixture(scope="session")
            def riglink_default_shell_root():
                return "rig"
        """)
    )
    pytester.makepyfile(test_x="""
        def test_shell_root(dev):
            # app-level default applied, no --riglink-shell-root given
            assert dev.captured_shell_root == "rig"
    """)
    result = pytester.runpytest("--riglink-port", "fake0", "-q")
    result.assert_outcomes(passed=1)


def test_shell_root_flag_overrides_app_level_default(pytester):
    """--riglink-shell-root still wins over the app-level default fixture."""
    pytester.makeconftest(
        "import pytest, riglink\n"
        + _FAKEDEV_PREAMBLE
        + _SESSION_PATCH_CAPTURE_SHELL_ROOT
        + textwrap.dedent("""
            @pytest.fixture(scope="session")
            def riglink_default_shell_root():
                return "rig"
        """)
    )
    pytester.makepyfile(test_x="""
        def test_shell_root(dev):
            assert dev.captured_shell_root == "other"
    """)
    result = pytester.runpytest("--riglink-port", "fake0",
                                "--riglink-shell-root", "other", "-q")
    result.assert_outcomes(passed=1)


# --- Extension hook: inject a device without --riglink-port -------------------
# Build a FakeDevice-backed riglink.Device in the consumer's conftest and hand
# it to the plugin via riglink_devices_factory. No connect() patch needed; the
# plugin must NOT require a port, yet still apply reset-scope + transcript hook.
_FACTORY_PREAMBLE = textwrap.dedent("""
    import pytest, riglink

    def _make_device():
        from tests._fakedev import FakeDevice
        fd = FakeDevice()
        fd.command("add2", "int", ["int", "int"])(lambda a, b: int(a) + int(b))
        d = riglink.Device(fd.transport)
        d._handshake()
        d._fakedev = fd   # expose reset_count so tests can assert reset-scope
        return d
""")


def test_factory_hook_injects_device_without_port(pytester):
    pytester.makeconftest(
        "import pytest, riglink\n"
        + _FAKEDEV_PREAMBLE
        + _FACTORY_PREAMBLE
        + textwrap.dedent("""
            @pytest.fixture(scope="session")
            def riglink_devices_factory():
                return {"native_sim": _make_device()}
        """)
    )
    pytester.makepyfile(test_x="""
        def test_add(dev):
            assert dev.add2(2, 3) == {"ret": 5}
    """)
    # NOTE: no --riglink-port; the factory supplies the device.
    result = pytester.runpytest("-q")
    result.assert_outcomes(passed=1)


def test_factory_hook_keeps_reset_scope(pytester):
    """The injected device still gets the plugin's per-function reset()."""
    pytester.makeconftest(
        "import pytest, riglink\n"
        + _FAKEDEV_PREAMBLE
        + _FACTORY_PREAMBLE
        + textwrap.dedent("""
            @pytest.fixture(scope="session")
            def riglink_devices_factory():
                return {"native_sim": _make_device()}
        """)
    )
    # FakeDevice counts rig.reset calls in reset_count. The plugin resets per
    # function by default, so the count must equal the test ordinal (1 before
    # test_a, 2 before test_b) — proving the injected device gets reset-scope.
    pytester.makepyfile(test_x="""
        def test_a(dev):
            assert dev._fakedev.reset_count == 1
        def test_b(dev):
            assert dev._fakedev.reset_count == 2
    """)
    result = pytester.runpytest("-q", "-p", "no:cacheprovider")
    result.assert_outcomes(passed=2)


def test_factory_hook_keeps_transcript_on_failure(pytester):
    """The injected device's transcript is attached to a failing test."""
    pytester.makeconftest(
        "import pytest, riglink\n"
        + _FAKEDEV_PREAMBLE
        + _FACTORY_PREAMBLE
        + textwrap.dedent("""
            @pytest.fixture(scope="session")
            def riglink_devices_factory():
                return {"native_sim": _make_device()}
        """)
    )
    pytester.makepyfile(test_x="""
        def test_fails(dev):
            assert dev.add2(2, 3) == 999
    """)
    result = pytester.runpytest("-q")
    result.assert_outcomes(failed=1)
    result.stdout.fnmatch_lines(["*riglink transcript*", "*send: add2 2 3*"])


def test_default_port_hook_reuses_connect(pytester):
    """riglink_default_port supplies a port; the plugin connects it via the
    patched connect() (so this also proves connect-config threading)."""
    pytester.makeconftest(
        "import pytest, riglink\n"
        + _FAKEDEV_PREAMBLE
        + _SESSION_PATCH_CAPTURE_SHELL_ROOT
        + textwrap.dedent("""
            @pytest.fixture(scope="session")
            def riglink_default_port():
                return "native-sim-pty"
        """)
    )
    pytester.makepyfile(test_x="""
        def test_shell_root(dev):
            # connected via the default-port hook, shell_root untouched
            assert dev.captured_shell_root is None
    """)
    # No --riglink-port: the default-port hook provides one.
    result = pytester.runpytest("-q")
    result.assert_outcomes(passed=1)


def test_no_port_no_hook_still_skips(pytester):
    """Backward compat: no port and no override => skip, as before."""
    pytester.makepyfile(test_x="def test_x(dev): pass")
    result = pytester.runpytest("-q")
    result.assert_outcomes(skipped=1)
