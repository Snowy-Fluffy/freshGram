# Publishing the plugin runtime

The app downloads the plugin host, Wasmtime and CPython together as one bundle when
the user turns plugins on. Nothing of it ships in the installer.

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
(`linux-x86_64`) to `url` (https only), `sha256` and `size`.

```
fgpl_tool keygen runtime
fgpl_tool sign-runtime runtime.json runtime.key runtime.signed.json
```

Upload `runtime.signed.json` as `runtime.json` next to the bundles. Build the app with
`-DFRESHGRAM_RUNTIME_KEY=<public key printed by keygen>`; without it the app refuses every
download. Keep `runtime.key` offline.
