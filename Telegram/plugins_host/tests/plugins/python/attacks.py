import os
import struct
import sys

import freshgram


def probe(action):
    try:
        return "ok:" + str(action())[:40]
    except BaseException as error:
        return "error:" + type(error).__name__


def escape():
    def connect():
        import socket
        sock = socket.socket()
        sock.connect(("127.0.0.1", 80))
        return "connected"

    def spawn():
        return os.system("id")

    def native():
        import ctypes
        return ctypes.CDLL("libc.so.6")

    return {
        "passwd": probe(lambda: open("/etc/passwd").read()),
        "maps": probe(lambda: open("/proc/self/maps").read()),
        "root": probe(lambda: os.listdir("/")),
        "dotdot": probe(lambda: os.listdir("/plugin/..")),
        "write": probe(lambda: open("/plugin/new.txt", "w").write("x")),
        "write_tmp": probe(lambda: open("/tmp/new.txt", "w").write("x")),
        "remove": probe(lambda: os.remove("/plugin/main.py")),
        "mkdir": probe(lambda: os.mkdir("/plugin/dir")),
        "socket": probe(connect),
        "spawn": probe(spawn),
        "native": probe(native),
        "environ": sorted(os.environ.keys()),
    }


def handle(kind, value):
    attack = value.get("attack")
    if attack == "garbage":
        freshgram._output.write(b"\xff" * 32)
        freshgram._output.flush()
    elif attack == "oversized":
        freshgram._output.write(struct.pack(">I", 200 * 1024 * 1024))
        freshgram._output.flush()
    elif attack == "flood":
        errors = 0
        for i in range(1000):
            reply = freshgram.call({"i": i})
            if isinstance(reply, dict) and reply.get("ok") is False:
                errors += 1
        return errors
    elif attack == "escape":
        freshgram.call({"escape": escape()})
    elif attack == "spin":
        while True:
            pass
    elif attack == "memory":
        try:
            blob = bytearray(1024 * 1024 * 1024)
            return len(blob) // (1024 * 1024)
        except MemoryError:
            return 1
    elif attack == "exit":
        sys.exit(0)
    elif attack == "stdout":
        print("this goes to the log, not to the channel")
    return 0


freshgram.run(handle)
