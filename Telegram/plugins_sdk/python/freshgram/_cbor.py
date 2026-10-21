import struct


def encode(value):
    out = bytearray()
    _encode(value, out, 0)
    return bytes(out)


def decode(data):
    value, position = _decode(data, 0, 0)
    if position != len(data):
        raise ValueError("trailing data")
    return value


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


def _encode(value, out, depth):
    if depth > 24:
        raise ValueError("too deep")
    if value is None:
        out.append(0xF6)
    elif value is True:
        out.append(0xF5)
    elif value is False:
        out.append(0xF4)
    elif isinstance(value, int):
        if not -(1 << 63) <= value < (1 << 63):
            raise OverflowError("integer out of range")
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
            _encode(item, out, depth + 1)
    elif isinstance(value, dict):
        _head(out, 5, len(value))
        for key, item in value.items():
            if not isinstance(key, str):
                raise TypeError("map keys must be strings")
            _encode(key, out, depth + 1)
            _encode(item, out, depth + 1)
    else:
        raise TypeError("cannot encode " + type(value).__name__)


def _half(bits):
    sign = -1.0 if bits & 0x8000 else 1.0
    exponent = (bits >> 10) & 0x1F
    fraction = bits & 0x3FF
    if exponent == 0:
        return sign * fraction * 2.0 ** -24
    if exponent == 31:
        return float("nan") if fraction else sign * float("inf")
    return sign * (fraction | 0x400) * 2.0 ** (exponent - 25)


def _decode(data, position, depth):
    if depth > 24:
        raise ValueError("too deep")
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
        raise ValueError("unsupported encoding")
    if major == 0:
        return argument, position
    if major == 1:
        return -1 - argument, position
    if major == 2:
        end = position + argument
        if end > len(data):
            raise ValueError("truncated")
        return bytes(data[position:end]), end
    if major == 3:
        end = position + argument
        if end > len(data):
            raise ValueError("truncated")
        return bytes(data[position:end]).decode("utf-8"), end
    if major == 4:
        items = []
        for _ in range(argument):
            item, position = _decode(data, position, depth + 1)
            items.append(item)
        return items, position
    if major == 5:
        result = {}
        for _ in range(argument):
            key, position = _decode(data, position, depth + 1)
            result[key], position = _decode(data, position, depth + 1)
        return result, position
    if major == 7:
        if info == 20:
            return False, position
        if info == 21:
            return True, position
        if info == 22:
            return None, position
        if info == 25:
            return _half(argument), position
        if info == 26:
            return struct.unpack(">f", struct.pack(">I", argument))[0], position
        if info == 27:
            return struct.unpack(">d", struct.pack(">Q", argument))[0], position
    raise ValueError("unsupported value")
