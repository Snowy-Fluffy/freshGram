#!/usr/bin/env python3
"""Fails when the host binary depends on anything but the system.

usage: check_host.py <binary>
The host runs in a sandbox that cannot load a library from anywhere else.
"""
import re
import subprocess
import sys

WINDOWS_ALLOWED = re.compile(
    r"^(kernel32|ntdll|advapi32|userenv|ws2_32|shell32|ole32|bcrypt|user32|"
    r"crypt32|secur32|oleaut32|ucrtbase|api-ms-win-.*)\.dll$",
    re.IGNORECASE)
MAC_ALLOWED = re.compile(r"^(/usr/lib/|/System/Library/)")


def windows(binary):
    output = subprocess.run(
        ["dumpbin", "/dependents", binary],
        check=True, capture_output=True, text=True).stdout
    return [line.strip() for line in output.splitlines()
            if line.strip().lower().endswith(".dll")
            and not WINDOWS_ALLOWED.match(line.strip())]


def mac(binary):
    output = subprocess.run(
        ["otool", "-L", binary],
        check=True, capture_output=True, text=True).stdout
    names = [line.split()[0] for line in output.splitlines()[1:] if line.strip()]
    return [name for name in names if not MAC_ALLOWED.match(name)]


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    binary = sys.argv[1]
    bad = windows(binary) if sys.platform == "win32" else mac(binary)
    for name in bad:
        print(f"{binary} depends on {name}", file=sys.stderr)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
