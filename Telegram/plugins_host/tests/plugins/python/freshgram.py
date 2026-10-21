import struct
import sys

_input = sys.stdin.buffer
_output = sys.stdout.buffer
sys.stdout = sys.stderr


def _encode(value, out):
    if value is None:
        out.append(0xF6)
    elif value is True:
        out.append(0xF5)
    elif value is False:
        out.append(0xF4)
    elif isinstance(value, int):
        if value >= 0:
            _head(out, 0, value)
        else:
            _head(out, 1, -1 - value)
    elif isinstance(value, float):
        out.append(0xFB)
        out += struct.pack(">d", value)
    elif isinstance(value, (bytes, bytearray)):
        _head(out, 2, len(value))
        out += value
    elif isinstance(value, str):
        data = value.encode("utf-8")
        _head(out, 3, len(data))
        out += data
    elif isinstance(value, (list, tuple)):
        _head(out, 4, len(value))
        for item in value:
            _encode(item, out)
    elif isinstance(value, dict):
        _head(out, 5, len(value))
        for key, item in value.items():
            _encode(str(key), out)
            _encode(item, out)
    else:
        raise TypeError("cannot encode " + type(value).__name__)


def _head(out, major, argument):
    lead = major << 5
    if argument < 24:
        out.append(lead | argument)
    elif argument <= 0xFF:
        out.append(lead | 24)
        out.append(argument)
    elif argument <= 0xFFFF:
        out.append(lead | 25)
        out += struct.pack(">H", argument)
    elif argument <= 0xFFFFFFFF:
        out.append(lead | 26)
        out += struct.pack(">I", argument)
    else:
        out.append(lead | 27)
        out += struct.pack(">Q", argument)


def _decode(data, position):
    initial = data[position]
    position += 1
    major = initial >> 5
    info = initial & 31
    if info < 24:
        argument = info
    elif info == 24:
        argument = data[position]
        position += 1
    elif info == 25:
        argument = struct.unpack_from(">H", data, position)[0]
        position += 2
    elif info == 26:
        argument = struct.unpack_from(">I", data, position)[0]
        position += 4
    elif info == 27:
        argument = struct.unpack_from(">Q", data, position)[0]
        position += 8
    else:
        raise ValueError("unsupported")
    if major == 0:
        return argument, position
    if major == 1:
        return -1 - argument, position
    if major == 2:
        return bytes(data[position:position + argument]), position + argument
    if major == 3:
        end = position + argument
        return bytes(data[position:end]).decode("utf-8"), end
    if major == 4:
        items = []
        for _ in range(argument):
            item, position = _decode(data, position)
            items.append(item)
        return items, position
    if major == 5:
        result = {}
        for _ in range(argument):
            key, position = _decode(data, position)
            result[key], position = _decode(data, position)
        return result, position
    if major == 7:
        if info == 20:
            return False, position
        if info == 21:
            return True, position
        if info == 22:
            return None, position
        if info == 27:
            return struct.unpack(">d", struct.pack(">Q", argument))[0], position
    raise ValueError("unsupported")


def _read(count):
    data = b""
    while len(data) < count:
        chunk = _input.read(count - len(data))
        if not chunk:
            raise EOFError
        data += chunk
    return data


def _send(message):
    body = bytearray()
    _encode(message, body)
    _output.write(struct.pack(">I", len(body)) + bytes(body))
    _output.flush()


def _receive():
    size = struct.unpack(">I", _read(4))[0]
    message, _ = _decode(_read(size), 0)
    return message


def call(request):
    _send({"t": "call", "v": request})
    while True:
        message = _receive()
        if message["t"] == "reply":
            return message["v"]


def run(handler):
    while True:
        try:
            message = _receive()
        except EOFError:
            return
        if message["t"] in ("init", "event"):
            status = handler(message["t"], message["v"]) or 0
            _send({"t": "done", "status": status})
