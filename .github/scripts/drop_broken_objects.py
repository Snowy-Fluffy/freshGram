#!/usr/bin/env python3
"""Delete object files a killed or poisoned build left broken.

Usage: drop_broken_objects.py <build dir>

Prints the deleted paths relative to the build dir, one per line, so the
caller can rebuild exactly those outputs.
"""
import os
import struct
import sys

BITCODE_MAGICS = (b"BC\xc0\xde", b"\xde\xc0\x17\x0b")
MACHO_MAGICS = {
    b"\xce\xfa\xed\xfe": ("<", False),
    b"\xcf\xfa\xed\xfe": ("<", True),
    b"\xfe\xed\xfa\xce": (">", False),
    b"\xfe\xed\xfa\xcf": (">", True),
}
FAT_MAGIC = b"\xca\xfe\xba\xbe"
LC_SEGMENT = 0x1
LC_SEGMENT_64 = 0x19


def elf_ok(stream, size):
    header = stream.read(64)
    if len(header) < 52:
        return False
    wide = header[4] == 2
    order = "<" if header[5] == 1 else ">"
    if wide:
        if len(header) < 64:
            return False
        shoff = struct.unpack_from(order + "Q", header, 0x28)[0]
        shentsize, shnum = struct.unpack_from(order + "HH", header, 0x3A)
    else:
        shoff = struct.unpack_from(order + "I", header, 0x20)[0]
        shentsize, shnum = struct.unpack_from(order + "HH", header, 0x2E)
    return shoff + shentsize * shnum <= size


def macho_ok(stream, size, order, wide):
    header_size = 32 if wide else 28
    header = stream.read(header_size)
    if len(header) < header_size:
        return False
    ncmds, sizeofcmds = struct.unpack_from(order + "II", header, 16)
    if header_size + sizeofcmds > size:
        return False
    commands = stream.read(sizeofcmds)
    offset = 0
    for _ in range(ncmds):
        if offset + 8 > len(commands):
            return False
        cmd, cmdsize = struct.unpack_from(order + "II", commands, offset)
        if cmdsize < 8 or offset + cmdsize > len(commands):
            return False
        if cmd == LC_SEGMENT_64 and cmdsize >= 56:
            fileoff, filesize = struct.unpack_from(
                order + "QQ", commands, offset + 40)
            if fileoff + filesize > size:
                return False
        elif cmd == LC_SEGMENT and cmdsize >= 40:
            fileoff, filesize = struct.unpack_from(
                order + "II", commands, offset + 32)
            if fileoff + filesize > size:
                return False
        offset += cmdsize
    return True


def fat_ok(stream, size):
    header = stream.read(8)
    if len(header) < 8:
        return False
    count = struct.unpack_from(">I", header, 4)[0]
    table = stream.read(20 * count)
    if len(table) < 20 * count:
        return False
    for index in range(count):
        offset, length = struct.unpack_from(">II", table, 20 * index + 8)
        if offset + length > size:
            return False
    return True


def object_ok(path):
    size = os.path.getsize(path)
    with open(path, "rb") as stream:
        magic = stream.read(8)
        stream.seek(0)
        if magic[:4] == b"\x7fELF":
            return elf_ok(stream, size)
        if magic[:4] in MACHO_MAGICS:
            order, wide = MACHO_MAGICS[magic[:4]]
            return macho_ok(stream, size, order, wide)
        if magic[:4] == FAT_MAGIC:
            return fat_ok(stream, size)
        return magic[:4] in BITCODE_MAGICS


def main():
    if len(sys.argv) != 2:
        sys.stderr.write(__doc__)
        return 2
    root = sys.argv[1]
    if not os.path.isdir(root):
        return 0
    checked = 0
    for directory, _, names in os.walk(root):
        for name in names:
            if not name.endswith(".o"):
                continue
            path = os.path.join(directory, name)
            if os.path.islink(path):
                continue
            checked += 1
            try:
                if object_ok(path):
                    continue
            except OSError:
                pass
            os.remove(path)
            print(os.path.relpath(path, root))
            sys.stderr.write(f"Removed the broken object {path}\n")
    sys.stderr.write(f"Checked {checked} object files in {root}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
