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

API_VERSION = 2

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
    def toast(self, text):
        return self._call("ui.toast", _nothing, text=text)

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


class _Messages(_Namespace):
    def history(self, chat, limit=20, before=0, topic=0):
        return self._call(
            "messages.history",
            chat=chat,
            limit=limit,
            before=before,
            **_given(topic=topic))

    def send(self, chat, text, reply_to=0, topic=0, silent=False, schedule=0, markdown=False):
        """Send a message; schedule is a unix time, markdown parses **bold** and the like."""
        return self._call(
            "messages.send",
            _nothing,
            chat=chat,
            text=text,
            **_given(reply_to=reply_to, topic=topic, silent=silent, schedule=schedule),
            **_format(markdown))

    def send_media(self, chat, data, name, caption="", kind="auto", markdown=False,
                   reply_to=0, topic=0, silent=False, schedule=0, spoiler=False, duration=0):
        """Send a file (messages.send), up to 10 MiB.

        kind "auto" sends photos and videos as media, "file" as a document,
        "voice" an OGG/Opus voice message (needs duration in seconds).
        """
        return self._call(
            "messages.send_media",
            _nothing,
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
            id=message,
            text=text,
            **_format(markdown))

    def delete(self, chat, messages, everyone=True):
        """Delete messages you sent (messages.delete), for everyone when possible."""
        return self._call("messages.delete", _nothing, chat=chat, ids=list(messages), everyone=everyone)

    def react(self, chat, message, emoji=""):
        """Put an emoji reaction on a message, or remove yours with an empty emoji."""
        return self._call("messages.react", _nothing, chat=chat, id=message, emoji=emoji)

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
    def set_outgoing(self, text, markdown=False):
        """Replace the text of the message being sent (message.outgoing only)."""
        _request("messages.set_outgoing", text=text, **_format(markdown))


class _Users(_Namespace):
    def info(self, user):
        """Name, username, bot, premium, contact and online status of a user."""
        return self._call("users.info", id=user)


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
        """The file of a message: kind, mime, name and data (up to 8 MiB).

        With download=True a file that is not on this device yet is downloaded
        first (up to a minute; prefer freshgram.aio for that).
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
