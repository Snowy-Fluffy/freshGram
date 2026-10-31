#!/usr/bin/env python3
"""Adds a Windows or macOS bundle to a runtime directory made by build_bundle.sh.

usage: add_platform.py <rev-dir> <platform> <host-binary> <fgpl_tool>
  platform is windows-x86_64, windows-arm64, macos-x86_64 or macos-arm64;
  the host is the signed binary from the build-plugin-host workflow;
  fgpl_tool is a build that runs on this machine, for example build-rel/fgpl_tool
  (the one in build-bundle/ is built for Rocky Linux and only runs in its container).

python/ and sdk/ are taken from a Linux bundle in the directory: they are the
same bytes on every platform. The entry is appended to runtime.unsigned.json.
"""
import base64
import hashlib
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

PLATFORMS = {
    "windows-x86_64": "fgpl_plugin_host.exe",
    "windows-arm64": "fgpl_plugin_host.exe",
    "macos-x86_64": "fgpl_plugin_host",
    "macos-arm64": "fgpl_plugin_host",
}


def digest(data):
    return "sha256-" + base64.b64encode(hashlib.sha256(data).digest()).decode()


def main():
    if len(sys.argv) != 5 or sys.argv[2] not in PLATFORMS:
        print(__doc__)
        return 2
    directory, platform, host, tool = (pathlib.Path(sys.argv[1]), sys.argv[2],
                                       pathlib.Path(sys.argv[3]), sys.argv[4])
    index = directory / "runtime.unsigned.json"
    text = index.read_text()
    if f'"{platform}"' in text:
        print(f"{platform} is already in {index}", file=sys.stderr)
        return 1
    source = next(directory.glob("runtime-linux-*.zip"), None)
    if source is None:
        print("no Linux bundle to take python/ and sdk/ from", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory() as temporary:
        stage = pathlib.Path(temporary) / "stage"
        with zipfile.ZipFile(source) as bundle:
            for name in bundle.namelist():
                if name.startswith(("python/", "sdk/")):
                    bundle.extract(name, stage)
        host_name = PLATFORMS[platform]
        shutil.copyfile(host, stage / host_name)
        output = directory / f"runtime-{platform}.zip"
        subprocess.run([tool, "pack-runtime", str(stage), str(output)],
                       check=True, stdout=subprocess.DEVNULL)
        entry = (
            f'"{platform}":{{"post":POST_ID_{platform.upper().replace("-", "_")},'
            f'"sha256":"{digest(output.read_bytes())}","size":{output.stat().st_size},'
            f'"hostSha256":"{digest((stage / host_name).read_bytes())}",'
            f'"pythonSha256":"{digest((stage / "python" / "python.wasm").read_bytes())}"}}')
    updated, count = re.subn(r"\}\}\s*$", "," + entry + "}}", text.rstrip() + "\n", count=1)
    if count != 1:
        print("cannot find the end of the bundles map", file=sys.stderr)
        return 1
    index.write_text(updated)
    print(f"wrote {output}\npost the file and replace POST_ID_{platform.upper().replace('-', '_')} in {index}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
