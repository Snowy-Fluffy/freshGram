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

## Windows and macOS

The platforms are `windows-x86_64`, `windows-arm64`, `macos-x86_64` and `macos-arm64`, next to
`linux-x86_64` and `linux-aarch64`. `python/` and `sdk/` are the same bytes everywhere; only the
host differs, and it is built on the platform itself by the `Build plugin host` workflow, which
also runs the tests there. Its artifacts `fgpl_plugin_host-<platform>` are the input of
`runtime/add_platform.py`:

```
runtime/build_bundle.sh <revision>
runtime/add_platform.py ~/freshgram-runtime/rev-<revision> windows-x86_64 fgpl_plugin_host.exe build-bundle/fgpl_tool
```

It takes `python/` and `sdk/` from the Linux bundle of that directory, packs the new zip and appends
its entry to `runtime.unsigned.json`; post the zip and replace `POST_ID_<PLATFORM>` like for Linux.
All platforms share one index and one `revision`, so a new platform means a new revision.

The Wasmtime archives for every host are pinned with their SHA-256 in `runtime/wasmtime_pins.txt`;
`runtime/fetch_wasmtime.py <arch>-<os>` downloads one and checks it.

How the host is confined, the requirement being the same everywhere: it refuses to run unless the
filesystem, processes and resource limits are all restricted.

- Linux: Landlock, seccomp and rlimits, applied by the host to itself.
- Windows: the app starts it in an AppContainer without capabilities (no files, registry or network
  except the pipe it inherits), inside a Job Object that kills it with the app and limits memory,
  CPU time and the process count, with a handful of process mitigations on top (JIT stays allowed,
  Wasmtime needs it). The AppContainer is given read access to the host file only.
- macOS: the host applies a deny-default Seatbelt profile to itself and rlimits; `RLIMIT_AS` does
  nothing there, so the app watches the resident size and stops a host that grows past the limit.

Integrity of the started file: Linux copies the verified bytes into a sealed memfd, Windows keeps
the file open without write sharing, macOS runs a verified copy from a private directory. The macOS
one leaves a race against a process of the same user, which can edit the profile anyway.

The macOS host must be signed before it is hashed into the index (Apple Silicon starts nothing
unsigned), the workflow signs it ad-hoc with `runtime/host.entitlements` (JIT). A Windows host that
was signed with a certificate is hashed as signed too. Dependencies are checked by
`runtime/check_host.py`: the sandbox cannot load a library from anywhere but the system.

Not available: Mac App Store and Microsoft Store builds, which cannot start a downloaded program.

## Bundle

A zip with exactly these top-level entries:

- `fgpl_plugin_host` (`fgpl_plugin_host.exe` on Windows), the host binary linked with Wasmtime;
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

## Official plugins

Plugins signed with the freshGram plugins key show as official; any other plugin, signed by
someone else or not signed, shows as unofficial. The key is separate from the runtime one:

```
fgpl_tool keygen plugins
fgpl_tool pack <directory> <output.fgpl> --key plugins.key
```

Its public part is the default of `-DFRESHGRAM_PLUGINS_KEY`. Keep `plugins.key` offline.

## Fuzzing

Needs clang. The libraries are built with `-fsanitize=fuzzer-no-link` and UBSan aborts on the first hit.

```
cmake -S . -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DFGPL_FUZZ=ON
cmake --build build-fuzz -j
build-fuzz/fuzz_seed_gen tests/fuzz_corpus/broker tests/fuzz_corpus/ui
build-fuzz/fuzz_broker build-fuzz/corpus_broker tests/fuzz_corpus/broker -dict=tests/fuzz_corpus/broker.dict -timeout=20
build-fuzz/fuzz_ui build-fuzz/corpus_ui tests/fuzz_corpus/ui -dict=tests/fuzz_corpus/ui.dict -timeout=20
```

`fuzz_broker` takes a grants byte (everything, one selected chat, nothing) and up to 16 CBOR requests. It aborts if a response leaks a hidden chat, a call without permission has an effect, or a message goes to a chat outside the grant. Other targets: `fuzz_cbor`, `fuzz_vfs`, `fuzz_frame`, `fuzz_wasi`, `fuzz_package`.
