# Publishing the plugin runtime

The app downloads the plugin host, Wasmtime and CPython together as one bundle when
the user turns plugins on. Nothing of it ships in the installer. Both the index and the
bundles come over MTProto from the public channel @freshgram_runtime (the CMake option
`FRESHGRAM_RUNTIME_CHANNEL`), through the signed-in account and its proxy settings.

## Building it locally

```
runtime/build_bundle.sh <revision> [--min-revision N] [--out DIR] [--skip-tests]
```

The script needs Docker. It downloads the pinned Wasmtime C API and CPython for WASI into
`third_party/` when they are missing and checks their SHA-256, builds the host and the
tests in Rocky Linux 8 (`runtime/Dockerfile`, build tree `build-bundle/`), runs the tests,
and refuses a host that needs anything but glibc 2.28 or a library outside glibc. The
host then runs on any glibc-based distribution from that version on. The result goes to
`DIR/rev-<revision>/` (default `~/freshgram-runtime`), which must not exist yet:
`runtime-linux-x86_64.zip` and `runtime.unsigned.json` with `POST_ID` in place of the post
id. The script prints the remaining steps.

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
`sha256` and `size`.

```
fgpl_tool keygen runtime
fgpl_tool sign-runtime runtime.json runtime.key runtime.signed.json
```

## Publishing in the channel

1. Post each bundle zip as a file (not as media) and note the post ids.
2. Put those ids into `bundles.*.post`, then sign the index.
3. Post `runtime.signed.json` as a file and pin that post. The app reads the pinned post
   only, so pinning a new index is the release; older posts can stay.

Build the app with
`-DFRESHGRAM_RUNTIME_KEY=<public key printed by keygen>`; without it the app refuses every
download. Keep `runtime.key` offline.
