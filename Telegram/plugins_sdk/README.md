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
  `c/freshgram.h` is a freestanding C implementation of the protocol with
  helpers for common calls (`fg_send_text`, `fg_send_text_ids`, `fg_send_media`,
  `fg_edit_text`, `fg_edit_media`, `fg_react`, `fg_toast`, `fg_notify`, `fg_chat_info`, `fg_history`, `fg_fetch`,
  `fg_timer_set`, `fg_timer_clear`, sockets, storage); any other call is built with
  `fg_begin` and `fg_send`.

## Wire format

Everything is CBOR (RFC 8949 subset: integers, byte and text strings, arrays,
maps with text keys, booleans, null, floats; no tags, no indefinite lengths).

Request: `{"op": "<name>", "a": {<arguments>}}`.
Response: `{"ok": true, "v": <value>}` or `{"ok": false, "e": <code>, "m": <text>}`.

An awaitable request adds `"async": true`; see *Waiting without blocking*.

Error codes: `denied` (permission or scope), `invalid` (bad arguments),
`not_found`, `quota`, `limit` (size or rate), `unavailable`, `failed`,
`cancelled` (the user closed a file dialog).

## Calls (API level 5)

| Call | Permission | Notes |
|---|---|---|
| `meta.info` | none | `api` (the level), `plugin`, `granted` permissions and `chats`: `"all"` or the list of chat ids in scope |
| `log` `{text}` | none | goes to the plugin log |
| `storage.get/set/delete/list/usage` | `storage.private` | keys up to 128 bytes, values up to 1 MiB, 4096 keys, quota per plugin |
| `net.fetch` `{url, method, headers, body, timeout}` | `net.https` | https only, to any public host name or public IP (`https://8.8.8.8/`, `https://[2606:4700::1]/`); local names, private, loopback and link-local addresses are refused, also after DNS; forbidden headers (`Host`, `Cookie`, ...); redirects are re-checked; `Set-Cookie` is never returned; `body` and the answer's body up to 100 MiB each, `timeout` up to 300000 ms (default 10000); returns `{status, headers, header_list, body}`, where `headers` joins repeated headers with `, ` and `header_list` keeps every `[name, value]` pair |
| `net.tcp.connect` `{host, port, tls, timeout}` | `net.tcp` | a TCP connection, optionally TLS with certificate checks; returns `{id}`; at most 16 open |
| `net.udp.connect` `{host, port}` | `net.udp` | a UDP socket bound to one peer, only its datagrams are received; returns `{id}`; at most 16 open, 200 datagrams per second |
| `net.socket.send` `{id, data, timeout}` | the socket's permission | up to 1 MiB (TCP) or 65507 bytes (UDP); returns `{sent}` |
| `net.socket.recv` `{id, max, timeout}` | the socket's permission | waits up to `timeout` ms (0 polls); returns `{data, closed}` |
| `net.socket.close` `{id}` | the socket's permission | sockets also close when the plugin stops |
| `contacts.list` `{limit}` | `contacts.list` | id, name, username; no phone numbers |
| `folders.list` | `folders.list` | chat folders with the chats in scope |
| `settings.read` | `settings.read` | `language`, `theme`, `streamerMode` |
| `media.get` `{chat, message, thumbnail, download}` | `media.read` | the file of a message, up to 100 MiB: `kind`, `mime`, `name`, `data`; without `download` only a file already on this device (`not_found` otherwise), with `download: true` the app downloads it first (up to five minutes, `limit` for larger files; await it) |
| `media.list` `{chat, kind, limit, before}` | `media.read` | messages with media, newest first, the same fields as the history; `kind` is `any` (the recent history), `photo`, `video`, `photo_video`, `file`, `voice`, `round`, `audio` or `gif` (searched on the server) |
| `messages.set_outgoing` `{text, entities, format, silent, schedule, reply_to, topic}` | `messages.transform_outgoing` | only while handling `message.outgoing`; give at least one field; `format: "markdown"` turns `**bold**`, `__italic__`, `` `code` ``, `~~strike~~` and `\|\|spoiler\|\|` into formatting; `entities` is a list of `{type, offset, length}` (UTF-16 units, up to 256) for the formatting of the text: `type` is `bold`, `italic`, `underline`, `strike`, `code`, `pre` (with `language`), `spoiler`, `blockquote`, `text_url` (needs an http, https or tg `url`), `mention_name` (needs the user `id`) or `custom_emoji` (needs the document `id`); the types Telegram finds itself (`url`, `mention`, `hashtag`, ...) are accepted so that entities read from a message can be passed back, and are ignored; entities cannot be combined with `format: "markdown"`, a new `text` without `entities` is sent without formatting, and entities that do not fit the text are dropped; `schedule` is a unix time (0 sends now), `reply_to` a message id of the chat (0 for none), `topic` a forum topic; the app ignores a value it cannot use (a time out of range, a message that is not there) |
| `messages.drop_outgoing` `{reason}` | `messages.transform_outgoing` | only while handling `message.outgoing`; the message is not sent and the input field stays empty; a non-empty `reason` (up to 200 bytes) is shown as a toast with the plugin's name; the plugins after this one do not see the message |
| `ui.dialog.show` `{page}`, `ui.dialog.close` | `ui.dialog` | the same tree as a settings page, shown as a dialog with the plugin's name |
| `ui.window.show` `{id, page, mode}`, `ui.window.update` `{id, page}`, `ui.window.close` `{id}` | `ui.dialog` | a window of the plugin's own: `id` is a lowercase token of up to 32 characters, `mode` is `layer` (over the chat, the default) or `window` (a window of its own); up to 3 windows at a time; see *Windows* |
| `translate.reply` `{ticket, items, format, error}` | `translate.provide` | answers a `translate.request`: `items` is a list with one translation for each requested text (up to 32 KiB each, 96 KiB in all), `format` is `plain` (default) or `markdown`; or `error` (up to 200 bytes) instead; `not_found` for a ticket that is not yours or already answered |
| `plugin.unload` | none | stop this plugin after the current event to free its memory |
| `plugins.send` `{to, data}` | `plugins.message` | send any CBOR value up to 64 KiB to another plugin of the same account; `not_found` when it is not installed, enabled or granted, `denied` when it does not accept this sender |
| `plugins.peers` | `plugins.message` | ids of the plugins this plugin may send to |
| `chats.list` `{limit}`, `chat.info` `{chat}` | `chats.list`, `chat.info` | `{id, title, type, unread, pinned, archived}`; `type` is `user`, `bot`, `group`, `supergroup`, `forum` or `channel`; archived chats are included; secret chats, Saved Messages and the service chat 777000 are never returned |
| `messages.history` `{chat, limit, before, topic, from_user}` | `messages.history` | newest first, `before` is a message id (exclusive); loads from the server when the app has not loaded enough; `topic` reads a forum topic; `from_user` is a user id or `"me"` and keeps only the messages of that sender (the server is searched for it, so the limit is filled); limit up to 100; chat must be in scope; each message has `id, chat, from, date, text, out` and `topic`, `reply_to`, `media`, `deleted`, `entities` (the formatting of the text, see `messages.set_outgoing`; also the types Telegram finds itself: `url`, `mention`, `hashtag`, `cashtag`, `bot_command`, `email`, `phone`) and `buttons` (the rows of an inline keyboard, each `{text, type, url?}` with `type` `callback`, `url` or `other`) when they apply; deleted messages kept by the app come only with `messages.read_deleted` |
| `messages.send` `{chat, text, format, reply_to, topic, silent, schedule, custom_emoji, entities}` | `messages.send` | `custom_emoji` is a list of `{offset, length, id}` that show part of the text (UTF-16 units) as custom emoji; `entities` is the general form (a list of `{type, offset, length, ...}` as in `messages.set_outgoing`, plus `mention_name` with the user's `id`, who must be known to the app) and cannot be combined with `format: "markdown"`; `reply_to` is a message id in the chat, `topic` a forum topic, `schedule` a unix time from 10 seconds to a year ahead; `denied` when you cannot write there, `limit` during slow mode; answers `{local_id}` at once: a negative id that `messages.edit`, `edit_media`, `react` and `delete` accept like a server id (they wait until a message that is still sending gets its server id, and fail with `failed` when the send fails); with `messages.ids` the call waits for the server and answers `{id, local_id}` too (`id` is null when the server did not answer in time, which is not an error, so a retry cannot send twice); `local_id` is null when no message was created (a dice, a text that is only spaces); scheduled sends have no ids |
| `messages.send_media` `{chat, data, name, kind, caption, format, reply_to, topic, silent, schedule, spoiler, duration}` | `messages.send` | sends a file of up to 100 MiB (`data` is bytes, `name` its file name); `kind` `auto` (default) sends photos and videos as media and the rest as files, `file` always as a file, `voice` sends OGG/Opus as a voice message and needs `duration` in seconds; `denied` when the chat forbids that kind of media or messages there cost Stars; counts against the same limit as `messages.send`; answers the ids like `messages.send` |
| `messages.edit_media` `{chat, id, data, name, kind, caption, format, spoiler}` | `messages.edit` | replaces the media of a message you sent and may still edit, with a file of up to 100 MiB; `kind` is `auto` or `file` (not `voice`); `id` is a server or a local id; `caption` replaces the old one; waits until Telegram accepts the file (await it); `invalid` when the album or Telegram does not take the file, `denied` when the message cannot carry new media |
| `messages.edit` `{chat, id, text, format, entities}` | `messages.edit` | only messages you sent and may still edit; `id` may be a local id; `entities` as in `messages.send` |
| `messages.delete` `{chat, ids, everyone}` | `messages.delete` | up to 100 of your own messages, server or local ids; for everyone (the default) when Telegram allows it for all of them |
| `messages.react` `{chat, id, emoji}` or `{chat, id, custom_emoji}` | `messages.react` | sets an emoji or custom emoji (a document id) reaction; neither removes yours |
| `messages.reactions` `{chat, id, limit, emoji or custom_emoji}` | `messages.read` | who reacted: `[{user, name, date, emoji or custom_emoji or paid}]` (up to 100), optionally only one reaction |
| `stickers.send` `{chat, id, reply_to, topic, silent}` | `messages.send` | sends an installed or recent sticker by its document id; same limits as `messages.send`; `not_found` when the sticker is not among the installed sets and recents; `denied` when the chat forbids stickers |
| `stickers.search` `{emoji, limit}`, `stickers.recent` `{limit}` | `messages.send` | `[{id, emoji, set, animated, video}]` from the installed sets for an emoji, or the recently used ones |
| `messages.forward` `{from_chat, ids, to_chat, silent}` | `messages.send` and `messages.read` | up to 100 messages; both chats must be in scope |
| `messages.search` `{chat, query, limit}` | `messages.history` | searches the chat on the server; the same message fields as the history |
| `messages.get` `{chat, id}` | `messages.history` | one message by its server id, loaded from the server when the app does not have it; `not_found` for service messages and, without `messages.read_deleted`, deleted ones |
| `messages.reply_chain` `{chat, id, limit}` | `messages.history` | the messages that `id` replies to, nearest first, up to `limit` (default 5, at most 20); stops at a message that is gone |
| `messages.pinned` `{chat, limit}`, `messages.scheduled` `{chat}` | `messages.history` | the pinned messages (newest first) or your scheduled messages of the chat, asked from the server; a message that comes this way has no `entities` or `buttons` |
| `messages.scheduled_delete` `{chat, ids}` | `messages.delete` | deletes scheduled messages by the ids of `messages.scheduled` |
| `messages.pin` `{chat, id, silent}`, `messages.unpin` `{chat, id}` | `chats.manage` | `id` is a server id; `denied` when you cannot pin there |
| `chats.mute` `{chat, seconds, forever}` | `chats.manage` | mutes for `seconds` (up to a year) or `forever`; neither unmutes |
| `chats.archive` `{chat, archived}`, `chats.pin` `{chat, pinned}` | `chats.manage` | both default to `true`; `chats.pin` pins in the main chat list and answers `limit` when it is full |
| `users.block` `{id}`, `users.unblock` `{id}` | `contacts.manage` | the user must be known to the app |
| `contacts.add` `{id, first_name, last_name}` | `contacts.manage` | adds a known user to your contacts |
| `composer.insert` `{chat, text}` | `composer.draft` | puts text into the draft of the chat at its saved cursor (at the end without a draft) |
| `chats.mark_read` `{chat}` | `messages.send` | |
| `chats.action` `{chat, kind, topic}` | `messages.send` | `typing` (default), `upload_photo`, `upload_video`, `upload_file`, `record_voice`, `upload_voice`, `record_video`, `record_round`, `choose_sticker` or `cancel`; once per 3 seconds per chat |
| `users.info` `{id}` | `users.info` | `id, name, username, bot, premium, contact, self, status, last_seen, has_photo`; `status` is `online`, `offline` (with `last_seen`), `recently`, `week`, `month`, `long_ago`, `hidden` or `bot` |
| `users.resolve` `{username}` | `users.info` | a user found by `@username` (with or without `@`); returns the same fields as `users.info`; asks the server for unknown names, once per second |
| `bots.inline_query` `{bot, query, chat}` | `bots.inline` | asks an inline bot (`bot` is its `@username` or user id, `query` up to 256 bytes, may be empty; `chat` is optional and tells the bot where the query is made) and returns up to 50 results `[{query_id, id, title, description, text}]`; `text` is the text the bot would send and is empty for results that send media; nothing is sent to any chat; once per second; `not_found` when the bot is unknown or blocked, `failed` when it has no inline mode or does not answer; await it |
| `bots.send_inline_result` `{chat, query_id, id, silent, reply_to, topic}` | `bots.inline` and `messages.send` | sends a result of `bots.inline_query` (its `query_id` and `id`) to the chat; the same limits as `messages.send` |
| `bots.press_button` `{chat, id, row, column}` | `bots.inline` | presses a button of the inline keyboard of message `id` (see `buttons`); a callback button answers `{text, alert, url}` (`text` is what the bot shows), a link button answers its `url` without any request; once per second; await it |
| `users.photo` `{id}` | `users.info` | `{mime, data}`: the current profile photo in full size as JPEG bytes (`image/jpeg`), or MP4 bytes (`video/mp4`) when the user has a video avatar; falls back to the small cached JPEG if the server does not answer; `not_found` when the user has none |
| `channels.info` `{id}` | `chat.info` | `id, title, username, type, about, members, verified, scam, fake, has_photo`; `type` is `channel` or `supergroup`, `members` is absent when unknown; other chats give `not_found` |
| `channels.resolve` `{username}` | `chat.info` | the same fields for a channel found by `@username`; once per second |
| `channels.photo` `{id}` | `chat.info` | `{mime, data}` like `users.photo` |
| `notify.show` `{title, text}` | `notifications` | a system notification (a toast when the app's notifications are off); once per 5 seconds; clicking it opens Saved Messages |
| `clipboard.write` `{text}`, `clipboard.read` | `clipboard` | only within 10 seconds after a user action; up to 64 KiB |
| `files.pick` `{title, filter}` | `fs.user_selected` | awaitable only, after a user action; the user picks a file in the system dialog; returns `{name, data}` (up to 100 MiB) or nothing when cancelled; `filter` is like `Images (*.png *.jpg)` |
| `files.save` `{name, data}` | `fs.user_selected` | awaitable only, after a user action; asks where to save `data` (bytes or text, up to 100 MiB); returns `true`, or `false` when cancelled |
| `timers.set` `{id, delay, repeat}`, `timers.clear` `{id}`, `timers.list` | none | `delay` in ms, from 1000 to 7 days; a timer sends a `timer` event and starts a stopped plugin (activation `timer`); at most 32 per plugin and account; timers live while the app runs and are dropped when the plugin is disabled |
| `account.info` | `account.info` | no phone number |
| `stars.balance` | `stars.balance` | read only |
| `composer.draft`, `composer.set_draft` `{chat, text, replace}` | `composer.draft` | setting a draft never switches the chat; a non-empty draft is kept unless `replace: true` |
| `badges.set` `{user, items}`, `badges.clear` | `ui.badges` | icons or mini pictures next to the name of a user, group or channel (`user` is the peer id as in `chats.list`), see *Badges next to names*; up to 4 for a peer and 256 peers |
| `ui.settings.set` `{page}` | `ui.settings_page` | declares the plugin's settings page, see below; the manifest needs `contributes.settingsPage` |
| `ui.toast` `{text, action, style, duration}` | `ui.toast` | the plugin id is prefixed; `action` is `{id, label}` and adds a button (the plugin gets `toast.click` `{id}` when it is pressed and `toast.close` `{id, reason}` when the toast goes, `reason` is `action` or `timeout`); `style: "undo"` needs an action and labels the button "Undo" when `label` is empty; `duration` is 1 to 8 seconds (5 for a toast with a button); an action id is up to 32 bytes, a label up to 40 |
| `time.now` | none | the server's unix time in seconds, not the device clock |
| `ui.open_settings` | `ui.settings_page` | opens the plugin's own settings page; only within 10 seconds after a user action (a menu click, a command, a settings or dialog event), once for each action; the manifest needs `contributes.settingsPage` |
| `time.sleep` `{ms}` | none | awaitable only; up to 60000 ms |

Every call counts against a budget of 200 calls a second; a plugin may make at
most 256 calls while handling one event. Some calls have limits of their own:
`messages.send` allows 5 messages at once and then one a second, `ui.toast` one
toast every 2 seconds, and `ui.dialog.show` opens a dialog only within 10
seconds of a user action (a menu click, a command, a settings or dialog event)
or while the plugin's dialog is already open; `ui.window.show` follows the same
rule, and an open window can be updated at most ten times a second. A call over a limit fails with
`limit`. A request or an answer may be up to 128 MiB; a larger answer fails with
`limit` instead of stopping the plugin. `runtime.limits.eventTimeoutMs` is the plugin's
own time for an event: time spent waiting for the app (a download, a socket
read) does not count, but all the calls of one event together may wait at most
60 seconds. Network connections show up in the app's Network
panel, where the user can close any of them. Plugin traffic always goes directly;
it never uses the proxy configured in the app.

Files of up to 100 MiB go both ways: `net.fetch` bodies, `media.get`,
`messages.send_media`, `messages.edit_media` and `files.pick/save`. Await such calls, since a plain call
counts against the 60 seconds of its event, and give the plugin room in
`runtime.limits.memoryMb`: a file passes through its memory about three times
(the answer, the plugin's own copy and the next request). A C plugin built on
`c/freshgram.h` defines `FG_HEAP_BYTES` large enough for that.

A manifest has no OS or architecture fields: a plugin is Wasm and runs the same
everywhere. If it cannot work without some capability of the app, list it in
`compat.requires`; an app that lacks one refuses the plugin with a clear reason.
Capabilities are the permission ids (`net.udp`, `ui.dialog`, `plugins.message`,
...) and the menu surfaces `menu.message`, `menu.chat`, `menu.profile`,
`menu.composer`, `menu.main_menu`, `menu.tray`, `menu.selection` and `menu.shared_media` (missing where the system has
no tray).

Set `compat.testedWith` to the freshGram version you tested with: when the app's
major or minor version is newer, the app marks the plugin as outdated.

## Events

The host delivers `init` once and then `event` messages. A `message.*` event is
only delivered with `messages.read`, only for chats in scope, never for secret
chats, Saved Messages or chat 777000; `message.deleted` loses its text without
`messages.read_deleted`. A plugin only receives the `message.*` events it lists
in `contributes.events`.

Every `message.*` event carries `id`, `chat`, `chat_title`, `chat_type`, `from`, `from_kind` (the type of the sender: `user`, `bot`, `self`, `channel`, `supergroup`, `group` or `forum`; a post or an anonymous admin shows the channel or the group),
`sender_name`, `date`, `text`, `out` and `forwarded`, and when they apply
`reply_to`, `topic`, `grouped` (album id), `media` (`photo`, `video`, `round`,
`gif`, `voice`, `audio`, `sticker`, `file`, `poll`, `contact`, `location`,
`webpage`, `other`), `edit_date`, `via_bot`, and with these fields:
`sticker` (`{id, emoji, set, animated, video}`) for a sticker message,
`emoji_only` (true when the text is only emoji, so the message is a big emoji
and not a sticker or plain text), `entities` (the custom emoji of the text,
`{type: "custom_emoji", offset, length, id}`, UTF-16 units) and `reactions`
(a list of `{count, my}` and one of `emoji`, `custom_emoji` or `paid`).
- `message.new` comes once per message, with its server id. Your own outgoing
  messages come when the server accepts them, with `local_id` set, and with
  `by_plugin: true` when a plugin sent them, so a plugin can ignore its own
  messages. Scheduled, sponsored and "Recent actions" items never come.
- `message.edited` comes only when the text or the edit date changes.
- `message.deleted` comes once, also when the app keeps deleted messages.
- `message.reaction` comes when someone reacts to your message.

`chat.opened` `{chat, chat_title, topic}` comes when the user opens a chat; it
needs `chats.active` instead of `messages.read` and follows the same chat scope.

More events, each listed in `contributes.events` and needing its own permission.
The ones with a `chat` follow the plugin's chat scope and never come for secret
chats, Saved Messages or 777000. Nothing is reported about blocked users. While Ghost Mode is on
for the account, reads you make (`chat.read` with `outbox: false`) and
`app.focus`/`app.blur` are not reported, since Ghost Mode hides them from others.

| Event | Permission | Payload |
|---|---|---|
| `chat.typing` | `chats.activity` | `{chat, user, action, cancelled}`; `action` is `typing`, `upload_photo`, `upload_video`, `upload_file`, `record_voice`, `upload_voice`, `record_video`, `record_round`, `choose_sticker` or `other`; comes at most once every 3 seconds per chat, user and action; `topic` is added in a forum topic |
| `chat.read` | `chats.activity` | `{chat, read_till, outbox}`; `outbox` is true when the other side read your messages up to `read_till`, false when you read theirs (also when you read them on another device); only when `read_till` grows |
| `chat.member` | `chats.activity` | `{chat, user, joined}`; someone joined or left a group or supergroup (not for channels) |
| `user.status` | `users.presence` | `{user, status, last_seen}` with the values of `users.info`; not for yourself, bots, or users who hide their status |
| `app.focus`, `app.blur` | `app.activity` | `{}`; the app window got or lost the focus |
| `translate.request` | `translate.provide` | `{ticket, to, format, items}`: texts to translate into the language `to` (a two-letter code); see *Translator* |
`timer` `{id}` comes when a timer set with `timers.set` fires; it is not listed
in `contributes.events`.

Chat ids are the app's peer ids: users are their user id, basic groups and
channels have a type in the high bits. Use them as they are.

`message.outgoing` `{chat, text, entities, chat_type, silent, topic, reply_to, scheduled}`
(`topic`, `reply_to` and `scheduled` only when set; `entities` is the formatting of the text as in `messages.set_outgoing`, an empty list for plain text) comes to plugins with
`messages.transform_outgoing` and `contributes.outgoing` before a text message is
sent (never for secret chats, Saved Messages, 777000, business shortcuts, welcome templates or
suggested posts; messages with files are not hooked yet). The plugin may call
`messages.set_outgoing` to replace the text or the send options, or
`messages.drop_outgoing` to stop the message; other data calls are refused during
this event. `contributes.outgoing.filter` limits the event to `chats` (a list of
chat ids) and `chat_types` (`user`, `bot`, `self`, `group`, `supergroup`, `forum`,
`channel`); a message that does not match is not delayed by the plugin at all. The
plugins see a message one after another, each one the changes of those before. It has about a second; `contributes.outgoing.onFailure` says what
happens when it fails or is late: `pass` sends the text unchanged, `block` stops
the message and puts it back into the input field.

## Translator

A plugin with the `translate.provide` permission and `contributes.translator`
(`{"name": "DeepL", "languages": ["en", "ru"], "maxChars": 4096}`: `languages` is
informational, `maxChars` from 64 to 8192 is the longest text it takes, 4096 by
default) appears in the app's list of translators (Settings, Translator). When
the user picks it, the app asks it for every translation: the message bar, the
translate box and the input field. The plugin gets `translate.request`
`{ticket, to, format: "plain", items}` with up to 20 texts and 24 KiB at a
time, formatting removed, and answers with `translate.reply` within 20 seconds
(a plugin that is not running is started; its `activation.on`, when listed,
must contain `translate`). A plugin may be asked several times at once (up to
8 requests), so answer by ticket. In Python:

```python
@freshgram.on_translate
def translate(items, to):
    return [shout(text) for text in items]
```

`on_translate(markdown=True)` makes the answers parsed as markdown, and an
`async def` handler is run on the plugin's own loop. In C, answer with
`fg_translate_reply` or `fg_translate_error`.

## Waiting without blocking

A plain call waits for its answer, and the plugin handles nothing else
meanwhile. An awaitable call (`"async": true`) is answered at once with
`{"ticket": N}`; the app does the work in the background and later delivers
`call.result` `{ticket, ok, v}` or `{ticket, ok: false, e, m}` as an ordinary
event. Every call except `messages.set_outgoing`, `messages.drop_outgoing` and `plugin.unload` can be
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
`freshgram.messages.set_outgoing` or `drop_outgoing` before its first `await`. In C,
`fg_begin_async` starts a request and `fg_send_async` sends it and writes the
ticket to its out-parameter; `fg_sleep_async` starts a timer. The results come
to `fg_plugin_event`.

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

## Exporting settings

A plugin that keeps its settings in `storage` can let the user move them to another
device. The manifest lists the keys:

```json
"storage": { "schema": 2, "export": { "prefixes": ["settings."], "exclude": ["settings.token"] } }
```

`prefixes` (up to 32, at least one) pick the keys, `exclude` removes some of them. Then
the plugin's details have "Export settings" and "Import settings", which write and read a
`.fgpls` file: CBOR `{format: "freshgram.plugin-settings", v: 1, plugin, pluginVersion,
storageSchema, exported, items}` with the listed values. An import is refused for another
plugin, for a `storageSchema` newer than the installed `storage.schema` (raise it when your
keys change their meaning), for keys outside `export`, and when the values would not fit the
key and quota limits. After the user confirms, the plugin stops, the values are replaced and
it starts again. Keys that are not in the file stay as they are.

## Activation and unloading

`activation.on` lists what may start the plugin: `startup`, `event`, `command`,
`menu`, `settings`, `message`, `timer`, `translate`, `toast` (the default is all of them except `startup`). Plugins that
change outgoing messages or run in the background start with the app. A plugin
calls `plugin.unload` when it has nothing to do; it is stopped after the
current event and started again by its next activation, so it costs no memory
in between.

## UI contributions

A plugin never runs code in the app's windows: it sends a tree of data and the app draws it. It declares menu items and commands in the manifest
(`contributes.menus`, `contributes.commands`, each needing `ui.menu` / `ui.command`) and the
app draws them with the plugin's name next to the title. Every `composer` item becomes its
own button next to the message field, for every plugin; when there are more than three, the
last button opens a menu with the rest.

Icons: a menu item, and the `toggle`, `button`, `link`, `select`, `chat` and `page` items of a page,
take `icon`, either a built-in name or one of the plugin's own icons. On a page (not in a menu) an
icon may also be `peer:<chat id>`, the avatar of a chat (needs `chat.info` or `chats.list`, the chat
in scope; never for secret chats, Saved Messages, 777000 or in streamer mode), or `emoji:<id>`, a custom emoji; a
page may have at most 32 of those and the app draws what it has, else the default icon. The plugin lists its own
icons in `contributes.icons` as `{"name": "icons/name.svg"}` (up to 32 `.svg` or `.png` files
from `files`, each up to 64 KiB). The app draws every icon as a mask in its own icon color, so
draw them in one color on a transparent background, 24×24. Built-in names: `add`, `archive`,
`block`, `bot`, `cancel`, `chat`, `chats`, `clear`, `collapse`, `commands`, `copy`,
`customize`, `delete`, `devices`, `download`, `edit`, `emoji`, `expand`, `export`, `file`,
`folder`, `font`, `forward`, `gif`, `hide`, `hourglass`, `info`, `like`, `link`, `lock`,
`mark_read`, `mute`, `network`, `night`, `notifications`, `palette`, `permissions`, `phone`,
`photo`, `pin`, `premium`, `profile`, `qr`, `reactions`, `remove`, `reply`, `report`,
`restore`, `schedule`, `search`, `select`, `send`, `settings`, `share`, `show`, `silent`,
`sound_off`, `sound_on`, `spoiler`, `star`, `stats`, `stickers`, `storage`, `timer`,
`translate`, `unmute`, `unpin`, `users`, `zoom_in`, `zoom_out`. A menu item with an unknown
icon is refused at install; a page item with an unknown icon shows the default one.

```json
"contributes": {
  "icons": { "spark": "icons/spark.svg" },
  "menus": [
    { "surface": "composer", "id": "tr", "title": "Translate", "icon": "translate" },
    { "surface": "message", "id": "save", "title": "Save note", "icon": "spark" }
  ]
}
```

Surfaces: `message` (a message's context menu), `selection` (the menu of several selected
messages in a chat, a list of up to 100 messages of one chat), `shared_media` (the context menu
of an item, or of the selected items, in a chat's shared media), `chat`, `profile`, `composer`,
`main_menu` and `tray`. `selection` and `shared_media` are never offered for secret chats or Saved Messages.

An item may carry `"when": {...}` to appear only where it fits. `chat_type` (a list of
`user`, `bot`, `self`, `group`, `supergroup`, `forum`, `channel`) works on every surface that
has a chat. The message keys work on `message`, `selection` and `shared_media` only: `out`,
`has_text`, `has_media` and `forwarded` (booleans) and `media` (a list of `photo`, `video`,
`gif`, `round`, `voice`, `audio`, `file`, `sticker`, `poll`, `contact`, `location`,
`webpage`, `other`). `has_media` is false for a message that only has a link preview. For
several messages every one of them has to fit. The user can hide any item in the plugin's
details ("Menu items"); a hidden item is not offered.

Activations come back as events:

| Event | Fields |
|---|---|
| `menu.click` | `surface` (`message`, `chat`, `profile`, `main_menu`, `tray`, `composer`, `selection`, `shared_media`), `id`, and when the plugin may see the chat: `chat`, `message`, `from`, `text` (`text` only with `messages.read`); `selection` and `shared_media` carry `chat` and `ids` (message ids, `ids` only with `messages.read`) instead of `message`; the `composer` surface is a button next to the input field and needs `ui.composer_button`, its click also has `topic` and `reply_to` like `command` (the reply is then taken off the input field) and `button: "right"` when the user pressed it with the right mouse button (left clicks have no `button`; the "more" menu of a long row of buttons is left-click only) |
| `badge.click` | `id`, `user`, `surface` (`message`, `chat`, `profile` or `chats`), and `chat`, `message` for a message; sent when the user clicks a badge set with `badges.set`; needs `ui.badges`, and starts a stopped plugin like a menu click (`activation.on` needs `menu`) |
| `command` | `name`, `args` (the text after `/name`), `chat` and `topic` when in scope, `reply_to` when the user was replying to a message of that chat (the reply is then taken off the input field, so answer with `messages.send` and `reply_to`); the command is offered in the input field's autocomplete, a bot's command with the same name wins, and `/name@plugin.id` always reaches the plugin |
| `toast.click`, `toast.close` | `id` of the action offered with `ui.toast`; `toast.close` also has `reason`; only the ids the plugin offered (up to 16 at a time) come back, also after the plugin unloaded itself (an `activation.on` list must contain `toast`) |
| `settings.open` | the user opened the plugin's settings; send the page now (the plugin is started for it) |
| `settings.change` | `id`, `value` of a toggle, input, select, slider (a number) or chat picker (a chat id) |
| `settings.click` | `id` of a button or of one of the page's `buttons` |
| `dialog.change`, `dialog.click` | the same for a dialog shown with `ui.dialog.show` |
| `dialog.close` | the user closed the dialog |
| `window.change`, `window.click`, `window.close` | the same for a window shown with `ui.window.show`, with its `window` id; a click on a `list` entry has the entry's `id`, a change of `tabs` the id of the chosen tab |

The settings page is a tree sent with `ui.settings.set`: `{"title": ..., "items": [...], "buttons": [...]}` with up to
64 items (`id` is a lowercase token, unique within the page, buttons included):

| Item | Fields |
|---|---|
| `text` | `text` (up to 1000 bytes, new lines allowed) |
| `header` | `text`: a section title |
| `divider` | none |
| `link` | `label`, `url` (`https://` only), `icon`; opens in the browser |
| `progress` | `label`, `value` from 0 to 100 |
| `toggle` | `id`, `label`, `value`, `icon` |
| `button` | `id`, `label`, `style` (`default`, `primary`, `danger`), `icon` |
| `input` | `id`, `label`, `value`, `placeholder`, `multiline`, `password`, `max` (up to 4096 bytes) |
| `select` | `id`, `label`, `value`, `options: [{value, label}]` (up to 16), `icon` |
| `slider` | `id`, `label`, `min`, `max`, `step`, `value` (at most 1000 steps) |
| `chat` | `id`, `label`, `value` (a chat id or 0), `icon`: the user picks a chat |

Pages can nest and show more than a settings list. These items also work in dialogs and
windows (all ids stay unique across the whole page; at most 4 levels and 256 items in total):

| Item | Fields |
|---|---|
| `row` | `items`: 1 to 4 items side by side |
| `column` | `items`: a group of items |
| `card` | `title`, `items`: a titled group set off from the rest of the page |
| `tabs` | `id`, `value` (the chosen tab), `tabs: [{id, label, items}]` (up to 8); a change sends the tab id |
| `list` | `items: [{id, title, subtitle, icon}]` (up to 64): rows that can be clicked |
| `table` | `columns` (up to 6 titles), `rows` (up to 32 lists of texts, one per column) |
| `image` | `data` (a PNG up to 2048×2048 or a JPEG, up to 256 KiB), `width`, `height` (up to 512), `alt` |
| `code` | `text`: monospaced text |
| `page` | `id`, `label`, `icon`, `hint`, `disabled`, `items`: a row that opens a sub-page with these items in its own box; its ids are part of the same page, so events need no path |

`text` also takes `style`: `title`, `subtitle`, `muted`, `accent` or `danger`; colors and fonts
come from the app's theme.

Interactive items also take `hint` (a line of text under the item) and
`disabled`. `buttons` (up to 3 of `{id, label, style}`) are drawn at the bottom
of a dialog. The app validates the tree, shows it in its own window and keeps
the last version; send it again to change it: values the user is typing are
kept. Text cannot contain control or bidi override characters.

## Badges next to names

With `ui.badges` a plugin marks a user, a group or a channel (give the peer id, as `chats.list` shows it: a channel is `(2 << 48) + id`, a basic group `(1 << 48) + id`) with up to four small icons or pictures right of the
name: in the sender line of a message, the title of an open chat, the profile header and the
chat list. `badges.set` replaces the plugin's badges for one user; an empty list removes them:

```python
freshgram.badges.set(user_id, [
    {"id": "vip", "icon": "star", "tooltip": "VIP"},
    {"id": "logo", "image": png_bytes},
])

@freshgram.on_badge_click("vip")
def vip(event):
    freshgram.ui.toast("VIP user " + str(event["user"]))
```

`icon` is a built-in name or one of the plugin's own icons (drawn in the text color), `image` is
a PNG up to 128x128 or a JPEG, up to 64 KiB (drawn as it is); give one of them. `tooltip` is up
to 80 bytes. Ids are lowercase tokens, unique among the badges of a user. A plugin that is
turned off, or loses the permission, shows nothing. Badges live while the app runs: set them
again on `init` (the plugin then needs `startup` in `activation.on`). Never the service chat 777000.

## Windows

`ui.window.show` opens a window with the same tree as a dialog and gives it a name, so a
plugin can keep several (a main window, a log, a picker). Sending it again with the same
`id`, or calling `ui.window.update`, changes the open window and keeps what the user is
typing. The app adds the plugin's name to every window and draws all of it itself:
there is no HTML or script. The plugin hears `window.*` events with the `window` id, and
`window.close` when the user closes it. A window closes when the plugin stops or is
disabled, and does not open while the app is locked with a passcode.

```python
@freshgram.on_command("board")
def board(event):
    freshgram.ui.window("board", "Board", [
        {"type": "tabs", "id": "tabs", "tabs": [
            {"id": "todo", "label": "To do", "items": [
                {"type": "list", "items": [{"id": "t1", "title": "Write docs"}]}]},
            {"id": "info", "label": "Info", "items": [{"type": "text", "text": "Hello"}]},
        ]},
    ], mode="window")

@freshgram.on_window("board", "t1")
def clicked(event):
    freshgram.ui.close_window("board")
```

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

`freshgram.on_settings_open`, `freshgram.on_dialog(item_id)`, `freshgram.on_window(window_id, item_id)`,
`freshgram.on_timer(timer_id)` and `freshgram.on_chat_opened` register handlers
for the settings page, dialogs, timers and opened chats; one failing handler is
logged and does not stop the others. The namespaces follow the calls:
`messages.send(chat, text, reply_to=..., markdown=True)`,
`messages.send_media(chat, data, name, caption=...)`, `media.list`,
`media.get(..., download=True)`, `messages.edit`,
`messages.delete`, `messages.react`, `messages.forward`, `messages.search`,
`chats.mark_read`, `chats.action`, `users.info`, `messages.reactions`, `stickers.send/search/recent`, `users.resolve`, `users.photo`, `channels.info`, `channels.resolve`, `channels.photo`, `notify.show`,
`bots.inline_query(bot, query, chat=None)`, `bots.send_inline_result(chat, query_id, result)`, `bots.press_button(chat, message, row, column)`, `ui.open_settings()`, `messages.history(..., from_user="me")`,
`messages.set_outgoing(text, entities=[...])`, `messages.get/reply_chain/pinned/scheduled/scheduled_delete/pin/unpin`, `chats.mute/archive/pin`, `users.block/unblock`, `contacts.add`, `composer.insert`, `freshgram.time()`,
`clipboard.read/write`, `timers.set(id, seconds, repeat=False)`; `files.pick`
and `files.save` exist only in `freshgram.aio`.

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
