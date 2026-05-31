import textwrap


# Preamble inserted into every pytester conftest to make tests._fakedev importable.
# The sub-pytest runs in a temp dir; riglink is installed editable so its __file__
# points into the real python/ tree — we climb up to python/ and add it to sys.path.
_FAKEDEV_PREAMBLE = textwrap.dedent("""
    import sys, pathlib
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
