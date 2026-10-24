# freshGram plugin SDK

A plugin runs in a sandboxed host process. It has no files, sockets or
processes of its own: every action is a call to the app, and each call is
checked against the permissions the user granted.

## Two ways to write a plugin

- **Python** (`python/freshgram`): a program run by CPython compiled for WASI.
  The host runs it as a command; messages travel over stdin/stdout.
- **Any language that builds a Wasm reactor**: export `fg_alloc`, `fg_free`,
  `fg_init(ptr, len)` and `fg_on_event(ptr, len)`, and import
  `freshgram.call(req_ptr, req_len, out_ptr_slot, out_len_slot) -> status`.
  `c/freshgram.h` is a freestanding C implementation of all of it.

## Wire format

Everything is CBOR (RFC 8949 subset: integers, byte and text strings, arrays,
maps with text keys, booleans, null, floats; no tags, no indefinite lengths).

Request: `{"op": "<name>", "a": {<arguments>}}`.
Response: `{"ok": true, "v": <value>}` or `{"ok": false, "e": <code>, "m": <text>}`.

An awaitable request adds `"async": true`; see *Waiting without blocking*.

Error codes: `denied` (permission or scope), `invalid` (bad arguments),
`not_found`, `quota`, `limit` (size or rate), `unavailable`, `failed`.

## Calls (API level 1)

| Call | Permission | Notes |
|---|---|---|
| `meta.info` | none | API level, plugin id, granted permissions |
| `log` `{text}` | none | goes to the plugin log |
| `storage.get/set/delete/list/usage` | `storage.private` | keys up to 128 bytes, values up to 1 MiB, 4096 keys, quota per plugin |
| `net.fetch` `{url, method, headers, body, timeout}` | `net.https` | https only, to any public host name or public IP (`https://8.8.8.8/`, `https://[2606:4700::1]/`); local names, private, loopback and link-local addresses are refused, also after DNS; forbidden headers (`Host`, `Cookie`, ...); redirects are re-checked; `Set-Cookie` is never returned |
| `net.tcp.connect` `{host, port, tls, timeout}` | `net.tcp` | a TCP connection, optionally TLS with certificate checks; returns `{id}`; at most 16 open |
| `net.udp.connect` `{host, port}` | `net.udp` | a UDP socket bound to one peer, only its datagrams are received; returns `{id}`; at most 16 open, 200 datagrams per second |
| `net.socket.send` `{id, data, timeout}` | the socket's permission | up to 1 MiB (TCP) or 65507 bytes (UDP); returns `{sent}` |
| `net.socket.recv` `{id, max, timeout}` | the socket's permission | waits up to `timeout` ms (0 polls); returns `{data, closed}` |
| `net.socket.close` `{id}` | the socket's permission | sockets also close when the plugin stops |
| `contacts.list` `{limit}` | `contacts.list` | id, name, username; no phone numbers |
| `folders.list` | `folders.list` | chat folders with the chats in scope |
| `settings.read` | `settings.read` | `language`, `theme`, `streamerMode` |
| `media.get` `{chat, message, thumbnail}` | `media.read` | a file already downloaded on this device, up to 8 MiB: `kind`, `mime`, `name`, `data` |
| `messages.set_outgoing` `{text}` | `messages.transform_outgoing` | only while handling `message.outgoing` |
| `ui.dialog.show` `{page}`, `ui.dialog.close` | `ui.dialog` | the same tree as a settings page, shown as a dialog with the plugin's name |
| `plugin.unload` | none | stop this plugin after the current event to free its memory |
| `plugins.send` `{to, data}` | `plugins.message` | send any CBOR value up to 64 KiB to another plugin of the same account; `not_found` when it is not installed, enabled or granted, `denied` when it does not accept this sender |
| `plugins.peers` | `plugins.message` | ids of the plugins this plugin may send to |
| `chats.list`, `chat.info` | `chats.list`, `chat.info` | secret chats and the service chat 777000 are never returned |
| `messages.history` `{chat, limit, before}` | `messages.history` | limit up to 100; chat must be in scope |
| `messages.send` `{chat, text}` | `messages.send` | |
| `account.info` | `account.info` | no phone number |
| `stars.balance` | `stars.balance` | read only |
| `composer.draft`, `composer.set_draft` | `composer.draft` | |
| `ui.settings.set` `{page}` | `ui.settings_page` | declares the plugin's settings page, see below; the manifest needs `contributes.settingsPage` |
| `ui.toast` `{text}` | `ui.toast` | the plugin name is prefixed |
| `time.sleep` `{ms}` | none | awaitable only; up to 60000 ms |

Every call counts against a per-second budget; a plugin may make at most 256
calls while handling one event. `runtime.limits.eventTimeoutMs` is the plugin's
own time for an event: time spent waiting for the app (a download, a socket
read) does not count, but all the calls of one event together may wait at most
60 seconds. Network connections show up in the app's Network
panel, where the user can close any of them. Plugin traffic always goes directly;
it never uses the proxy configured in the app.

A manifest has no OS or architecture fields: a plugin is Wasm and runs the same
everywhere. If it cannot work without some capability of the app, list it in
`compat.requires`; an app that lacks one refuses the plugin with a clear reason.
Capabilities are the permission ids (`net.udp`, `ui.dialog`, `plugins.message`,
...) and the menu surfaces `menu.message`, `menu.chat`, `menu.profile`,
`menu.composer`, `menu.main_menu` and `menu.tray` (missing where the system has
no tray).

Set `compat.testedWith` to the freshGram version you tested with: when the app's
major or minor version is newer, the app marks the plugin as outdated.

## Events

The host delivers `init` once and then `event` messages. A `message.*` event is
only delivered with `messages.read`, only for chats in scope, never for secret
chats or chat 777000; `message.deleted` loses its text without
`messages.read_deleted`. A plugin only receives the `message.*` events it lists
in `contributes.events`.

`message.outgoing` `{chat, text}` comes to plugins with
`messages.transform_outgoing` and `contributes.outgoing` before a text message is
sent (never for secret chats or 777000). The plugin may call
`messages.set_outgoing` to replace the text; other data calls are refused during
this event. It has about a second; `contributes.outgoing.onFailure` says what
happens when it fails or is late: `pass` sends the text unchanged, `block` stops
the message and puts it back into the input field.

## Waiting without blocking

A plain call waits for its answer, and the plugin handles nothing else
meanwhile. An awaitable call (`"async": true`) is answered at once with
`{"ticket": N}`; the app does the work in the background and later delivers
`call.result` `{ticket, ok, v}` or `{ticket, ok: false, e, m}` as an ordinary
event. Every call except `messages.set_outgoing` and `plugin.unload` can be
awaited; at most 32 may wait at a time (`limit` after that), and none can be
started while handling `message.outgoing`. When the plugin stops, the calls
still waiting are dropped.

In Python, `freshgram.aio` has the same namespaces as `freshgram` with
awaitable calls, and handlers may be `async def`:

```python
import freshgram
from freshgram import aio

@freshgram.on_command("rates")
async def rates(event):
    usd, eur = await aio.gather(
        aio.net.get("https://example.org/usd"),
        aio.net.get("https://example.org/eur"))
    await aio.ui.toast(usd.text() + " / " + eur.text())

@freshgram.on("message.new")
def counted(event):
    # Runs while rates() waits for the network.
    freshgram.storage.set("seen", freshgram.storage.get("seen", 0) + 1)

freshgram.run()
```

`aio.sleep(seconds)`, `aio.gather(*awaitables)`, `aio.wait_for(awaitable,
timeout)` and `freshgram.spawn(coroutine)` (a background task whose failure is
logged) work on the plugin's own loop; `asyncio` does not run in the sandbox.
An async handler of `message.outgoing` must call
`freshgram.messages.set_outgoing` before its first `await`. In C,
`fg_begin_async` / `fg_send_async` return the ticket and `fg_sleep_async`
starts a timer; the results come to `fg_plugin_event`.

## Messages between plugins

Plugins never talk to each other directly: a message goes through the app, which
checks both sides. The sender needs `plugins.message`; the receiver needs
`plugins.message` too and lists who may write to it in
`contributes.messages.from` (plugin ids, or `"*"` for anyone). The receiver gets
`plugin.message` `{from, data}`, is started for it when `activation.on` includes
`message` (the default), and answers, if it wants to, with its own
`plugins.send`. Messages stay within one account.

```python
@freshgram.on_plugin_message("com.example.sender")
def got(sender, data):
    freshgram.plugins.send(sender, {"echo": data})
```

## Accounts

A plugin works the same in every account: the user grants its permissions once,
and its storage and settings are shared. Each signed-in account still gets its
own instance, and every event, call and message belongs to the account it came
from (`init` carries `account`). The instances of one plugin handle their events
one at a time, so storage updates from two accounts never interleave.

## Activation and unloading

`activation.on` lists what may start the plugin: `startup`, `event`, `command`,
`menu`, `settings`, `message` (the default is all of them except `startup`). Plugins that
change outgoing messages or run in the background start with the app. A plugin
calls `plugin.unload` when it has nothing to do; it is stopped after the
current event and started again by its next activation, so it costs no memory
in between.

## UI contributions

A plugin never draws widgets. It declares menu items and commands in the manifest
(`contributes.menus`, `contributes.commands`, each needing `ui.menu` / `ui.command`) and the
app draws them with the plugin's name next to the title. Activations come back as events:

| Event | Fields |
|---|---|
| `menu.click` | `surface` (`message`, `chat`, `profile`, `main_menu`, `tray`, `composer`), `id`, and when the plugin may see the chat: `chat`, `message`, `from`, `text` (`text` only with `messages.read`); the `composer` surface is a button next to the input field and needs `ui.composer_button` |
| `command` | `name`, `args` (the text after `/name`), `chat` when in scope |
| `settings.change` | `id`, `value` of a toggle, input or select |
| `settings.click` | `id` of a button |
| `dialog.change`, `dialog.click` | the same for a dialog shown with `ui.dialog.show` |
| `dialog.close` | the user closed the dialog |

The settings page is a tree sent with `ui.settings.set`: `{"title": ..., "items": [...]}` with up to
64 items of type `text`, `divider`, `toggle`, `input`, `select` and `button` (`id` is a lowercase
token, unique within the page). The app validates it, shows it in its own window and keeps the last
version; send it again to change it. Text cannot contain control or bidi override characters.

## Python

```python
import freshgram

@freshgram.on_init
def start(info):
    freshgram.storage.set("started", True)

@freshgram.on("message.*")
def message(event):
    count = freshgram.storage.get("count", 0) + 1
    freshgram.storage.set("count", count)

freshgram.run()
```

Calls raise `freshgram.Denied`, `Invalid`, `NotFound`, `QuotaExceeded`,
`LimitExceeded`, `Unavailable` or `Failed`. `print` goes to the plugin log; the
protocol channel is not reachable by accident.

## C

```c
#include "freshgram.h"

int fg_plugin_init(fg_value info) { fg_log("ready"); return 0; }

int fg_plugin_event(fg_value event) {
	fg_i64 count = 0;
	fg_storage_get_int("count", &count);
	fg_storage_set_int("count", count + 1);
	return 0;
}
```

Build: `clang --target=wasm32 -O2 -nostdlib -fno-builtin -Wl,--no-entry -o plugin.wasm plugin.c`.
