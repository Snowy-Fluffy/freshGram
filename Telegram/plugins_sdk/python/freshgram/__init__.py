"""The freshGram plugin SDK for Python.

A plugin is a Python program that the host runs inside a sandbox. It cannot
open files, sockets or processes: everything goes through the calls below, and
each call is checked against the permissions the user granted.
"""

import json as _json
import traceback as _traceback
import types as _types

from . import _aio
from . import _channel
from . import _cbor
from . import _errors

API_VERSION = 4

_output = _channel._output
call = _channel.raw_call

from ._errors import (  # noqa: E402
    FreshgramError,
    Denied,
    Invalid,
    NotFound,
    QuotaExceeded,
    LimitExceeded,
    Unavailable,
    Failed,
)


def _request(operation, **arguments):
    return _errors.from_response(_channel.raw_call({"op": operation, "a": arguments}))


def _sync_call(operation, then=None, **arguments):
    value = _request(operation, **arguments)
    return value if then is None else then(value)


def _async_call(operation, then=None, **arguments):
    return _aio.request(operation, arguments, then)


def info():
    """The API version, the plugin id and the granted permissions."""
    return _request("meta.info")


def _fit(text, limit=4000):
    data = str(text).encode("utf-8")
    if len(data) <= limit:
        return str(text)
    return data[-limit:].decode("utf-8", "ignore")


def log(text):
    _request("log", text=_fit(text))


def _log_exception():
    try:
        log(_traceback.format_exc())
    except Exception:
        pass


class _Namespace:
    def __init__(self, call):
        self._call = call


class _Storage(_Namespace):
    def get(self, key, default=None):
        return self._call("storage.get", lambda value: default if value is None else value, key=key)

    def set(self, key, value):
        return self._call("storage.set", _nothing, key=key, value=value)

    def delete(self, key):
        return self._call("storage.delete", key=key)

    def keys(self, prefix="", limit=100):
        return self._call("storage.list", prefix=prefix, limit=limit)

    def usage(self):
        return self._call("storage.usage")


def _nothing(value):
    return None


class Response:
    def __init__(self, data):
        self.status = data["status"]
        self.headers = data["headers"]
        self.header_list = [tuple(pair) for pair in data.get("header_list", [])]
        self.body = data["body"]

    def text(self, encoding="utf-8"):
        return self.body.decode(encoding, "replace")

    def json(self):
        return _json.loads(self.text())

    @property
    def ok(self):
        return 200 <= self.status < 300


class Socket:
    """A TCP or UDP connection opened through the app.

    Hosts may be names or public IP addresses; local and private
    addresses are refused. Timeouts are in milliseconds. A socket opened
    through freshgram.aio returns awaitables from send, recv and close.
    """

    def __init__(self, socket_id, call=_sync_call):
        self.id = socket_id
        self._call = call

    def send(self, data, timeout=10000):
        return self._call("net.socket.send", lambda value: value["sent"], id=self.id, data=data, timeout=timeout)

    def recv(self, max_bytes=65507, timeout=1000):
        """Return (data, closed); data is empty when nothing arrived in time."""
        return self._call(
            "net.socket.recv",
            lambda value: (value["data"], value["closed"]),
            id=self.id,
            max=max_bytes,
            timeout=timeout)

    def close(self):
        return self._call("net.socket.close", _nothing, id=self.id)

    def __enter__(self):
        return self

    def __exit__(self, *details):
        self.close()

    async def __aenter__(self):
        return self

    async def __aexit__(self, *details):
        await self.close()


class _Net(_Namespace):
    def tcp(self, host, port, tls=False, timeout=10000):
        """Open a TCP connection (net.tcp), optionally wrapped in TLS."""
        return self._call(
            "net.tcp.connect",
            lambda value: Socket(value["id"], self._call),
            host=host,
            port=port,
            tls=tls,
            timeout=timeout)

    def udp(self, host, port, timeout=10000):
        """Open a UDP socket bound to one peer (net.udp)."""
        return self._call(
            "net.udp.connect",
            lambda value: Socket(value["id"], self._call),
            host=host,
            port=port,
            timeout=timeout)

    def fetch(self, url, method="GET", headers=None, body=None, timeout=10000):
        arguments = {"url": url, "method": method, "timeout": timeout}
        if headers:
            arguments["headers"] = headers
        if body is not None:
            arguments["body"] = body
        return self._call("net.fetch", Response, **arguments)

    def get(self, url, **options):
        return self.fetch(url, "GET", **options)

    def post(self, url, body=None, **options):
        return self.fetch(url, "POST", body=body, **options)


class _Ui(_Namespace):
    def toast(self, text, action=None, label=None, undo=False, duration=None):
        """Show a toast. With an action id the toast gets a button (label, or "Undo"
        for undo=True) and on_toast(action) runs when it is pressed.

        duration is in seconds, from 1 to 8 (5 for a toast with a button).
        """
        arguments = {}
        if action is not None:
            arguments["action"] = {"id": action, **_given(label=label)}
        if undo:
            arguments["style"] = "undo"
        if duration is not None:
            arguments["duration"] = duration
        return self._call("ui.toast", _nothing, text=text, **arguments)

    def settings_page(self, title, items, buttons=None):
        """Describe the plugin's settings page; the app draws it.

        Items are dicts: text, header, divider, link, toggle, button, input,
        select, slider, chat and progress. See the README for their fields.
        """
        return self._call("ui.settings.set", _nothing, page=_page(title, items, buttons))

    def dialog(self, title, items, buttons=None):
        """Show a dialog (ui.dialog); clicks arrive as dialog.* events.

        buttons is a list of {"id", "label", "style"} drawn at the bottom.
        """
        return self._call("ui.dialog.show", _nothing, page=_page(title, items, buttons))

    def close_dialog(self):
        return self._call("ui.dialog.close", _nothing)

    def window(self, window_id, title, items, buttons=None, mode="layer"):
        """Show a window or update it (ui.dialog); events carry its id as `window`.

        Items may nest: row, column, card and tabs hold other items, and
        image, list, table and code add content. mode is "layer" (over the
        chat) or "window" (a window of its own); an open window keeps its mode.
        """
        return self._call(
            "ui.window.show", _nothing,
            id=window_id, mode=mode, page=_page(title, items, buttons))

    def update_window(self, window_id, title, items, buttons=None):
        return self._call(
            "ui.window.update", _nothing,
            id=window_id, page=_page(title, items, buttons))

    def close_window(self, window_id):
        return self._call("ui.window.close", _nothing, id=window_id)


def _page(title, items, buttons):
    page = {"title": title, "items": items}
    if buttons:
        page["buttons"] = buttons
    return page


def _given(**arguments):
    return {key: value for key, value in arguments.items() if value}


def _format(markdown):
    return {"format": "markdown"} if markdown else {}


class _Chats(_Namespace):
    def list(self, limit=100):
        return self._call("chats.list", limit=limit)

    def info(self, chat):
        return self._call("chat.info", chat=chat)

    def mark_read(self, chat):
        """Mark the chat as read (messages.send)."""
        return self._call("chats.mark_read", _nothing, chat=chat)

    def action(self, chat, kind="typing", topic=0):
        """Show "typing…" or another action in the chat, "cancel" to stop."""
        return self._call("chats.action", _nothing, chat=chat, kind=kind, **_given(topic=topic))


class Sent:
    """What messages.send and messages.send_media answer.

    local_id is known at once and stays valid until the app restarts; id is
    the server id, which needs the messages.ids permission (the call then
    waits for it) and is None without it, or when the server did not answer
    in time. Pass either, or this object, to edit, react and delete.
    """

    def __init__(self, data):
        data = data or {}
        self.id = data.get("id")
        self.local_id = data.get("local_id")

    def __repr__(self):
        return "Sent(id=%r, local_id=%r)" % (self.id, self.local_id)


def _ref(message):
    if isinstance(message, Sent):
        return message.id or message.local_id
    return message


class _Messages(_Namespace):
    def history(self, chat, limit=20, before=0, topic=0):
        return self._call(
            "messages.history",
            chat=chat,
            limit=limit,
            before=before,
            **_given(topic=topic))

    def send(self, chat, text, reply_to=0, topic=0, silent=False, schedule=0, markdown=False):
        """Send a message; schedule is a unix time, markdown parses **bold** and the like.

        Returns Sent. With messages.ids the call waits for the server, so await it
        (freshgram.aio) instead of blocking the event.
        """
        return self._call(
            "messages.send",
            Sent,
            chat=chat,
            text=text,
            **_given(reply_to=reply_to, topic=topic, silent=silent, schedule=schedule),
            **_format(markdown))

    def send_media(self, chat, data, name, caption="", kind="auto", markdown=False,
                   reply_to=0, topic=0, silent=False, schedule=0, spoiler=False, duration=0):
        """Send a file (messages.send), up to 100 MiB.

        kind "auto" sends photos and videos as media, "file" as a document,
        "voice" an OGG/Opus voice message (needs duration in seconds).
        Returns Sent, see send.
        """
        return self._call(
            "messages.send_media",
            Sent,
            chat=chat,
            data=bytes(data),
            name=name,
            kind=kind,
            **_given(
                caption=caption,
                reply_to=reply_to,
                topic=topic,
                silent=silent,
                schedule=schedule,
                spoiler=spoiler,
                duration=duration),
            **_format(markdown))

    def edit(self, chat, message, text, markdown=False):
        """Edit a message you sent (messages.edit)."""
        return self._call(
            "messages.edit",
            _nothing,
            chat=chat,
            id=_ref(message),
            text=text,
            **_format(markdown))

    def edit_media(self, chat, message, data, name, caption="", kind="auto",
                   markdown=False, spoiler=False):
        """Replace the media of a message you sent (messages.edit), up to 100 MiB.

        kind "auto" or "file"; caption replaces the old one. Await it: the call
        returns when Telegram has accepted the new file.
        """
        return self._call(
            "messages.edit_media",
            _nothing,
            chat=chat,
            id=_ref(message),
            data=bytes(data),
            name=name,
            kind=kind,
            **_given(caption=caption, spoiler=spoiler),
            **_format(markdown))

    def delete(self, chat, messages, everyone=True):
        """Delete messages you sent (messages.delete), for everyone when possible."""
        return self._call("messages.delete", _nothing, chat=chat, ids=[_ref(message) for message in messages], everyone=everyone)

    def react(self, chat, message, emoji=""):
        """Put an emoji reaction on a message, or remove yours with an empty emoji."""
        return self._call("messages.react", _nothing, chat=chat, id=_ref(message), emoji=emoji)

    def forward(self, from_chat, messages, to_chat, silent=False):
        """Forward messages (messages.send and messages.read)."""
        return self._call(
            "messages.forward",
            _nothing,
            from_chat=from_chat,
            ids=list(messages),
            to_chat=to_chat,
            **_given(silent=silent))

    def search(self, chat, query, limit=20):
        """Search the chat on the server (messages.history)."""
        return self._call("messages.search", chat=chat, query=query, limit=limit)


class _SyncMessages(_Messages):
    def set_outgoing(self, text=None, markdown=False, silent=None, schedule=None,
                     reply_to=None, topic=None):
        """Change the message being sent (message.outgoing only).

        Give the new text, and any of silent, schedule (a unix time, 0 to send
        now), reply_to (a message id, 0 for none) and topic. A value the app
        cannot use, such as a message that is not there, is ignored.
        """
        arguments = {}
        if text is not None:
            arguments["text"] = text
            arguments.update(_format(markdown))
        for key, value in (("silent", silent), ("schedule", schedule),
                           ("reply_to", reply_to), ("topic", topic)):
            if value is not None:
                arguments[key] = value
        _request("messages.set_outgoing", **arguments)

    def drop_outgoing(self, reason=""):
        """Do not send the message being sent (message.outgoing only).

        The input field stays empty. A non-empty reason is shown as a toast.
        """
        _request("messages.drop_outgoing", **_given(reason=reason))


class _Users(_Namespace):
    def info(self, user):
        """Name, username, bot, premium, contact and online status of a user."""
        return self._call("users.info", id=user)


class _Badges(_Namespace):
    def set(self, user, items):
        """Show icons or mini pictures next to the name of a user (ui.badges).

        items is a list of up to 4 dicts {"id", "icon" or "image", "tooltip"}: icon is a
        built-in or the plugin's own icon name, image is PNG or JPEG bytes (up to 128x128
        and 64 KiB). An empty list removes the badges. A click comes as on_badge_click.
        """
        return self._call("badges.set", _nothing, user=user, items=list(items))

    def clear(self):
        """Remove every badge this plugin has set."""
        return self._call("badges.clear", _nothing)


class _Notify(_Namespace):
    def show(self, text, title=""):
        """Show a system notification (notifications), at most one in 5 seconds."""
        return self._call("notify.show", _nothing, text=text, **_given(title=title))


class _Clipboard(_Namespace):
    """The clipboard (clipboard), only within 10 seconds after a user action."""

    def write(self, text):
        return self._call("clipboard.write", _nothing, text=text)

    def read(self):
        return self._call("clipboard.read")


class _Files(_Namespace):
    """Files the user picks (fs.user_selected); only awaitable, see freshgram.aio."""

    def pick(self, title="", filter=""):
        """Ask the user for a file: {"name", "data"}, or None if they cancel."""
        return self._call("files.pick", **_given(title=title, filter=filter))

    def save(self, name, data):
        """Ask where to save data; True when saved, False when cancelled."""
        return self._call("files.save", name=name, data=data)


class _Timers(_Namespace):
    """Timers that wake the plugin with a timer event, even after it stopped.

    They live while the app runs: at most 32, from 1 second to 7 days.
    """

    def set(self, timer_id, seconds, repeat=False):
        return self._call(
            "timers.set",
            _nothing,
            id=timer_id,
            delay=int(seconds * 1000),
            **_given(repeat=repeat))

    def clear(self, timer_id):
        return self._call("timers.clear", id=timer_id)

    def list(self):
        return self._call("timers.list")


class _Plugins(_Namespace):
    def send(self, to, data=None):
        """Send data to another plugin (plugins.message on both sides).

        The other plugin must list this plugin (or "*") in
        contributes.messages.from; it receives a plugin.message event.
        """
        return self._call("plugins.send", _nothing, to=to, data=data)

    def peers(self):
        """Plugins this plugin may send messages to."""
        return self._call("plugins.peers")


class _Contacts(_Namespace):
    def list(self, limit=200):
        return self._call("contacts.list", limit=limit)


class _Folders(_Namespace):
    def list(self):
        return self._call("folders.list")


class _Settings(_Namespace):
    def read(self):
        return self._call("settings.read")


class _Media(_Namespace):
    def get(self, chat, message, thumbnail=False, download=False):
        """The file of a message: kind, mime, name and data (up to 100 MiB).

        With download=True a file that is not on this device yet is downloaded
        first (up to five minutes; prefer freshgram.aio for that).
        """
        return self._call(
            "media.get",
            chat=chat,
            message=message,
            thumbnail=thumbnail,
            **_given(download=download))

    def list(self, chat, kind="any", limit=20, before=0):
        """Messages with media, newest first: photo, video, photo_video, file,
        voice, round, audio, gif or any."""
        return self._call("media.list", chat=chat, kind=kind, limit=limit, **_given(before=before))


class _Account(_Namespace):
    def info(self):
        return self._call("account.info")


class _Stars(_Namespace):
    def balance(self):
        return self._call("stars.balance", lambda value: value["balance"])


class _Composer(_Namespace):
    def draft(self, chat):
        return self._call("composer.draft", chat=chat)

    def set_draft(self, chat, text):
        return self._call("composer.set_draft", _nothing, chat=chat, text=text)


storage = _Storage(_sync_call)
net = _Net(_sync_call)
ui = _Ui(_sync_call)
chats = _Chats(_sync_call)
messages = _SyncMessages(_sync_call)
account = _Account(_sync_call)
stars = _Stars(_sync_call)
composer = _Composer(_sync_call)
plugins = _Plugins(_sync_call)
contacts = _Contacts(_sync_call)
folders = _Folders(_sync_call)
settings = _Settings(_sync_call)
media = _Media(_sync_call)
users = _Users(_sync_call)
notify = _Notify(_sync_call)
badges = _Badges(_sync_call)
clipboard = _Clipboard(_sync_call)
timers = _Timers(_sync_call)


def spawn(coroutine):
    """Run a coroutine on the plugin's loop; see freshgram.aio."""
    return _aio.spawn(coroutine)


def unload():
    """Stop this plugin after the current event to free its memory.

    The app starts it again on the next event, command, menu click or
    settings change it is activated by.
    """
    _request("plugin.unload")

_init_handlers = []
_event_handlers = []


def on_init(function):
    """Run function(info) when the plugin starts; it may be async."""
    _init_handlers.append(function)
    return function


def on(pattern):
    """Run the function for events whose type matches; it may be async.

    An async handler runs until its first await while the event is being
    handled, the rest after the awaited calls are answered. Other events are
    handled meanwhile.
    """
    def register(function):
        _event_handlers.append((pattern, function))
        return function
    return register


def on_menu(menu_id, surface=None):
    def register(function):
        def wrapper(event):
            if event.get("id") == menu_id and (surface is None or event.get("surface") == surface):
                return function(event)
        _event_handlers.append(("menu.click", wrapper))
        return function
    return register


def on_badge_click(badge_id=None):
    """Run the function(event) when the user clicks a badge set with badges.set.

    The event has id, user, surface (message, chat, profile or chats) and, for
    a message, chat and message.
    """
    def register(function):
        def wrapper(event):
            if badge_id is None or event.get("id") == badge_id:
                return function(event)
        _event_handlers.append(("badge.click", wrapper))
        return function
    return register


def on_command(name):
    def register(function):
        def wrapper(event):
            if event.get("name") == name:
                return function(event)
        _event_handlers.append(("command", wrapper))
        return function
    return register


def on_plugin_message(sender=None):
    def register(function):
        def wrapper(event):
            if sender is None or event.get("from") == sender:
                return function(event.get("from"), event.get("data"))
        _event_handlers.append(("plugin.message", wrapper))
        return function
    return register


def on_setting(item_id=None):
    def register(function):
        def wrapper(event):
            if event.get("type") == "settings.open":
                return None
            if item_id is None or event.get("id") == item_id:
                return function(event)
        _event_handlers.append(("settings.*", wrapper))
        return function
    return register


def on_settings_open(function):
    """Run when the user opens the plugin's settings; send the page here."""
    _event_handlers.append(("settings.open", function))
    return function


def on_dialog(item_id=None):
    """Run for clicks and changes in the plugin's dialog, and when it closes."""
    def register(function):
        def wrapper(event):
            if item_id is None or event.get("id") == item_id:
                return function(event)
        _event_handlers.append(("dialog.*", wrapper))
        return function
    return register


def on_window(window_id, item_id=None):
    """Run for clicks and changes in a window of ui.window, and when it closes."""
    def register(function):
        def wrapper(event):
            if event.get("window") == window_id and (item_id is None or event.get("id") == item_id):
                return function(event)
        _event_handlers.append(("window.*", wrapper))
        return function
    return register


def on_timer(timer_id=None):
    """Run when a timer set with timers.set fires."""
    def register(function):
        def wrapper(event):
            if timer_id is None or event.get("id") == timer_id:
                return function(event)
        _event_handlers.append(("timer", wrapper))
        return function
    return register


def on_chat_opened(function):
    """Run when the user opens a chat (chats.active, contributes.events)."""
    _event_handlers.append(("chat.opened", function))
    return function


def on_translate(function=None, markdown=False):
    """Make the plugin a translator (translate.provide, contributes.translator).

    The function gets (items, to): a list of up to 20 texts and the target
    language code, and returns the list of translations, one for each item,
    or a coroutine of it. Raise an exception to answer with an error. With
    markdown=True the answers use **bold** and the like. A plugin whose
    manifest lists activation triggers needs "translate" among them.
    """
    def register(handler):
        def wrapper(event):
            ticket = event["ticket"]

            def reply(items=None, error=None):
                if error is not None:
                    _request("translate.reply", ticket=ticket, error=str(error)[:200])
                else:
                    _request(
                        "translate.reply",
                        ticket=ticket,
                        items=[str(item) for item in items],
                        **_format(markdown))

            try:
                result = handler(event["items"], event["to"])
            except Exception as error:
                _log_exception()
                reply(error=error)
                return

            if isinstance(result, _types.CoroutineType):
                async def finish():
                    try:
                        items = await result
                    except Exception as error:
                        _log_exception()
                        reply(error=error)
                        return
                    reply(items)
                _aio.spawn(finish())
            else:
                reply(result)
        _event_handlers.append(("translate.request", wrapper))
        return handler
    return register(function) if function is not None else register


def on_toast(action_id=None):
    """Run when the button of a toast shown by ui.toast(..., action=...) is pressed.

    The event has the action id. Without an id every button of the plugin
    is handled.
    """
    def register(function):
        def wrapper(event):
            if action_id is None or event.get("id") == action_id:
                return function(event)
        _event_handlers.append(("toast.click", wrapper))
        return function
    return register


def on_toast_close(action_id=None):
    """Run when a toast with a button goes away; the event has the action id
    and reason: "action" after a press, "timeout" otherwise."""
    def register(function):
        def wrapper(event):
            if action_id is None or event.get("id") == action_id:
                return function(event)
        _event_handlers.append(("toast.close", wrapper))
        return function
    return register


def on_typing(function):
    """Run when someone types or sends something in a chat (chats.activity).

    The event has chat, user, action ("typing", "record_voice", ...) and
    cancelled (true when they stopped).
    """
    _event_handlers.append(("chat.typing", function))
    return function


def on_read(function):
    """Run when messages of a chat are read (chats.activity).

    The event has chat, read_till (a message id) and outbox: true when the
    other side read your messages, false when you read theirs.
    """
    _event_handlers.append(("chat.read", function))
    return function


def on_member(function):
    """Run when someone joins or leaves a group or channel (chats.activity).

    The event has chat, user and joined.
    """
    _event_handlers.append(("chat.member", function))
    return function


def on_user_status(function):
    """Run when a user's online status changes (users.presence).

    The event has user, status (as in users.info) and last_seen. Statuses
    that a user hides, bots and your own are not reported.
    """
    _event_handlers.append(("user.status", function))
    return function


def on_app_focus(function):
    """Run when the app window gets the focus (app.activity)."""
    _event_handlers.append(("app.focus", function))
    return function


def on_app_blur(function):
    """Run when the app window loses the focus (app.activity)."""
    _event_handlers.append(("app.blur", function))
    return function


def _matches(pattern, name):
    if pattern.endswith("*"):
        return name.startswith(pattern[:-1])
    return pattern == name


def _start(result):
    if isinstance(result, _types.CoroutineType):
        _aio.spawn(result)


def _call_each(functions, value):
    status = 0
    for function in functions:
        try:
            _start(function(value))
        except Exception:
            _log_exception()
            status = -1
    return status


def _handle(kind, value):
    if kind == "init":
        return _call_each(_init_handlers, value)
    name = value.get("type", "") if isinstance(value, dict) else ""
    if name == "call.result":
        _aio.resolve(value)
        return 0
    return _call_each(
        [function for pattern, function in _event_handlers if _matches(pattern, name)],
        value)


def run(handler=None):
    """Handle events until the host stops the plugin."""
    while True:
        try:
            message = _channel.receive()
        except EOFError:
            return
        except ValueError:
            _log_exception()
            return
        kind = message.get("t")
        if kind not in ("init", "event"):
            continue
        value = message.get("v")
        try:
            if (isinstance(value, dict) and value.get("type") == "call.result"):
                _aio.resolve(value)
                status = 0
            elif handler is not None:
                status = handler(kind, value) or 0
            else:
                status = _handle(kind, value) or 0
        except Exception:
            _log_exception()
            status = -1
        _aio.run_ready()
        _channel.send({"t": "done", "status": int(status)})
