import sys
import pytest
import riglink
from riglink import cli
from tests._fakedev import FakeDevice


@pytest.fixture
def patched_connect(monkeypatch):
    fd = FakeDevice()
    fd.command("add2", "int", ["int", "int"])(lambda a, b: int(a) + int(b))

    def _connected(fd):
        d = riglink.Device(fd.transport)
        d._handshake()
        return d

    monkeypatch.setattr(cli, "connect", lambda *a, **k: _connected(fd))
    return fd


def test_cli_list(patched_connect, capsys):
    assert cli.main(["list", "--port", "x"]) == 0
    out = capsys.readouterr().out
    assert "add2" in out and "rig.echo" in out


def test_cli_call(patched_connect, capsys):
    assert cli.main(["call", "--port", "x", "add2", "2", "3"]) == 0
    assert capsys.readouterr().out.strip() == "5"


def test_cli_call_unknown(patched_connect, capsys):
    rc = cli.main(["call", "--port", "x", "frob"])
    assert rc == 1
    assert "error" in capsys.readouterr().err.lower()


def test_cli_stubs(patched_connect, capsys):
    assert cli.main(["stubs", "--port", "x"]) == 0
    out = capsys.readouterr().out
    assert "class RiglinkDevice" in out
    assert "def add2(self, a0: int, a1: int) -> int: ..." in out
