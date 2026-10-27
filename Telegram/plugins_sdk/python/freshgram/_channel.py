import struct
import sys

from . import _cbor

_input = sys.stdin.buffer
_output = sys.stdout.buffer
sys.stdout = sys.stderr

MAX_FRAME = 128 << 20


def _read(count):
    data = b""
    while len(data) < count:
        chunk = _input.read(count - len(data))
        if not chunk:
            raise EOFError("the host closed the channel")
        data += chunk
    return data


def send(message):
    body = _cbor.encode(message)
    if len(body) > MAX_FRAME:
        raise ValueError("the message is too large")
    _output.write(struct.pack(">I", len(body)) + body)
    _output.flush()


def receive():
    size = struct.unpack(">I", _read(4))[0]
    if size > MAX_FRAME:
        raise ValueError("the frame is too large")
    return _cbor.decode(_read(size))


def raw_call(request):
    send({"t": "call", "v": request})
    while True:
        message = receive()
        if message.get("t") == "reply":
            return message.get("v")
