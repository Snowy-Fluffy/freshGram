#!/usr/bin/env python3
"""Downloads a Wasmtime C API release for the host build and checks it against wasmtime_pins.txt.

usage: fetch_wasmtime.py <arch>-<os> [--print-hash]
       arch is x86_64 or aarch64, os is linux, macos or windows.

Unpacks into third_party/wasmtime-v<version>-<arch>-<os>-c-api, where CMake looks for it.
--print-hash only downloads and prints the line to add to the pins file.
"""
import hashlib
import pathlib
import sys
import tarfile
import urllib.request
import zipfile

VERSION = "50.0.0"
HERE = pathlib.Path(__file__).resolve().parent
THIRD_PARTY = HERE.parent / "third_party"
URL = "https://github.com/bytecodealliance/wasmtime/releases/download/v{version}/{name}"


def asset_name(target):
    arch, system = target.split("-", 1)
    extension = "zip" if system == "windows" else "tar.xz"
    return f"wasmtime-v{VERSION}-{arch}-{system}-c-api.{extension}"


def pinned(name):
    for line in (HERE / "wasmtime_pins.txt").read_text().splitlines():
        if line.strip() and not line.startswith("#"):
            pinned_name, digest = line.split()
            if pinned_name == name:
                return digest
    return None


def main():
    arguments = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(arguments) != 1:
        print(__doc__)
        return 2
    name = asset_name(arguments[0])
    THIRD_PARTY.mkdir(exist_ok=True)
    archive = THIRD_PARTY / name
    if not archive.exists():
        part = archive.with_suffix(archive.suffix + ".part")
        with urllib.request.urlopen(URL.format(version=VERSION, name=name)) as response:
            part.write_bytes(response.read())
        part.rename(archive)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if "--print-hash" in sys.argv:
        print(f"{name} {digest}")
        return 0
    expected = pinned(name)
    if expected is None:
        print(f"{name} is not pinned in wasmtime_pins.txt", file=sys.stderr)
        return 1
    if digest != expected:
        print(f"{name} does not match the pinned SHA-256", file=sys.stderr)
        return 1
    target = THIRD_PARTY / name.removesuffix(".tar.xz").removesuffix(".zip")
    if not target.exists():
        if name.endswith(".zip"):
            with zipfile.ZipFile(archive) as bundle:
                bundle.extractall(THIRD_PARTY)
        else:
            with tarfile.open(archive) as bundle:
                bundle.extractall(THIRD_PARTY)
    print(target)
    return 0


if __name__ == "__main__":
    sys.exit(main())
