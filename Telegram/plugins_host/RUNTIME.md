# Publishing the plugin runtime

The app downloads the plugin host, Wasmtime and CPython together as one bundle when
the user turns plugins on. Nothing of it ships in the installer. Both the index and the
bundles come over MTProto from the public channel @freshgram_runtime (the CMake option
`FRESHGRAM_RUNTIME_CHANNEL`), through the signed-in account and its proxy settings.

## Building it locally

```
runtime/build_bundle.sh <revision> [--arch x86_64|aarch64] [--min-revision N] [--out DIR] [--skip-tests]
```

The script needs Docker. It downloads the pinned Wasmtime C API releases and CPython for
WASI into `third_party/` when they are missing and checks their SHA-256, and builds in
Rocky Linux 8 (`runtime/Dockerfile`): the x86_64 host natively (`build-bundle/`) and the
aarch64 host with clang against an aarch64 Rocky 8 sysroot (`runtime/aarch64.cmake`,
`build-bundle-aarch64/`). It refuses a host that needs anything but glibc 2.28 or a library
outside glibc, so each host runs on any glibc-based distribution from that version on.

Tests: the whole x86_64 suite runs in the container. For aarch64 the suites that do not
need the sandbox run under `qemu-aarch64` when it is installed; the sandbox suites need an
aarch64 kernel (real hardware or a `qemu-system-aarch64` machine), because qemu-user does not
run seccomp or Landlock. On aarch64 the seccomp filter also allows the process-local
`membarrier` commands that Wasmtime uses to sync the instruction cache after JIT writes.

The result goes to `DIR/rev-<revision>/` (default `~/freshgram-runtime`), which must not
exist yet: one `runtime-linux-<arch>.zip` per architecture and one `runtime.unsigned.json`
with `POST_ID_X86_64` and `POST_ID_AARCH64` in place of the post ids. The script prints the
remaining steps.

## Bundle

A zip with exactly these top-level entries:

- `fgpl_plugin_host`, the host binary linked with Wasmtime;
- `python/python.wasm` and `python/lib/...`, CPython for WASI and its standard library;
- `sdk/freshgram/...`, the Python SDK from `plugins_sdk/python`.

```
fgpl_tool pack-runtime <directory> runtime-linux-x86_64.zip
```

The command prints the `sha256` and `size` fields for the index.

## Index

`runtime.json` is a JSON object with `schema` 1, `revision` (grows with every release),
`minRevision` (older revisions are refused and cannot be installed again), `host`,
`wasmtime`, `python`, an optional `revoked` list and `bundles`, a map from a platform name
(`linux-x86_64`) to `post` (the id of the channel post that carries the bundle as a file),
`sha256` and `size` of the zip, and `hostSha256` and `pythonSha256` of `fgpl_plugin_host`
and `python/python.wasm` inside it.

The app keeps the signed index next to the installed files and trusts only the hashes in
it. Before the first plugin starts it reads the host, checks it against `hostSha256`, copies
it into a sealed memfd and starts every host from that copy, so replacing the file on disk
afterwards changes nothing.

```
fgpl_tool keygen runtime
fgpl_tool sign-runtime runtime.json runtime.key runtime.signed.json
```

## Updates

The app checks the index a minute after start and then once a day. With "Update components
automatically" on (the default) a newer revision is downloaded, installed and the plugins
restart on it; with it off the settings only show that an update exists.

## Offline and packaged runtimes

"Install components from file" takes a signed `runtime.json`; the bundle must lie next to
it as `runtime-<platform>.zip`, the same pair that is posted in the channel. Packages that
must not download executables (Flathub, distribution packages) build the app with
`-DFRESHGRAM_RUNTIME_DIR=<directory>` and ship that pair there: the app then installs and
updates the runtime only from this directory and never contacts the channel.

## Publishing in the channel

1. Post each bundle zip as a file (not as media) and note the post ids; one index lists
   the bundles of every architecture.
2. Put those ids into `bundles.*.post`, then sign the index.
3. Post `runtime.signed.json` as a file and pin that post. The app reads the pinned post
   only, so pinning a new index is the release; older posts can stay.

Build the app with
`-DFRESHGRAM_RUNTIME_KEY=<public key printed by keygen>`; without it the app refuses every
download. Keep `runtime.key` offline.
