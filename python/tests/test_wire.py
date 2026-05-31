import base64
import pytest
from riglink._wire import classify, encode_call, encode_token


@pytest.mark.parametrize("value, expected", [
    (0, "0"), (-22, "-22"), (True, "true"), (False, "false"),
    (3.5, "3.5"), ("abc", "abc"), ("a b", '"a b"'), ("", '""'),
    ("#x", '"#x"'), ('a"b\\c', '"a\\"b\\\\c"'),
])
def test_encode_token(value, expected):
    assert encode_token(value) == expected


def test_encode_token_bytes_is_base64():
    assert encode_token(b"Hi") == base64.b64encode(b"Hi").decode()


def test_encode_call():
    assert encode_call("foo", [3, 4]) == b"foo 3 4\n"
    assert encode_call("greet", ["hello world", 7]) == b'greet "hello world" 7\n'
    assert encode_call("rig.list") == b"rig.list\n"


def test_classify_response():
    assert classify(b'\x1eRIG {"cmd":"foo","ret":0}') == ("response", {"cmd": "foo", "ret": 0})


def test_classify_error_is_a_response():
    kind, obj = classify(b'\x1eRIG {"cmd":"foo","error":{"code":"unknown_cmd"}}')
    assert kind == "response" and obj["error"]["code"] == "unknown_cmd"


def test_classify_event():
    assert classify(b'\x1eRIG {"event":"rx_done","len":12}\n') == ("event", {"event": "rx_done", "len": 12})


def test_classify_log():
    assert classify(b'\x1eRIG {"log":"hi"}') == ("log", "hi")


def test_classify_console_passthrough():
    assert classify(b"[00:00:01.234] <inf> app: hello") == ("console", "[00:00:01.234] <inf> app: hello")


def test_classify_malformed_sentinel_line():
    kind, _ = classify(b'\x1eRIG {not json')
    assert kind == "malformed"


def test_encode_token_rejects_control_chars():
    with pytest.raises(ValueError):
        encode_token("a\nb")
    with pytest.raises(ValueError):
        encode_token("x\ty")
