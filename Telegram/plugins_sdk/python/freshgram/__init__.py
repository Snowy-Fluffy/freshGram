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

API_VERSION = 1

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


def log(text):
    _request("log", text=str(text))


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

    def settings_page(self, title, items):
        """Describe the plugin's settings page; the app draws it.

        Items are dicts: text, divider, toggle, input, select and button.
        """
        return self._call("ui.settings.set", _nothing, page={"title": title, "items": items})

    def dialog(self, title, items):
        """Show a dialog (ui.dialog); clicks arrive as dialog.* events."""
        return self._call("ui.dialog.show", _nothing, page={"title": title, "items": items})

    def close_dialog(self):
        return self._call("ui.dialog.close", _nothing)


class _Chats(_Namespace):
    def list(self, limit=100):
        return self._call("chats.list", limit=limit)

    def info(self, chat):
        return self._call("chat.info", chat=chat)


class _Messages(_Namespace):
    def history(self, chat, limit=20, before=0):
        return self._call("messages.history", chat=chat, limit=limit, before=before)

    def send(self, chat, text):
        return self._call("messages.send", _nothing, chat=chat, text=text)


class _SyncMessages(_Messages):
    def set_outgoing(self, text):
        """Replace the text of the message being sent (message.outgoing only)."""
        _request("messages.set_outgoing", text=text)


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
    def get(self, chat, message, thumbnail=False):
        """A downloaded file of a message: kind, mime, name and data."""
        return self._call("media.get", chat=chat, message=message, thumbnail=thumbnail)


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
            if item_id is None or event.get("id") == item_id:
                return function(event)
        _event_handlers.append(("settings.*", wrapper))
        return function
    return register


def _matches(pattern, name):
    if pattern.endswith("*"):
        return name.startswith(pattern[:-1])
    return pattern == name


def _start(result):
    if isinstance(result, _types.CoroutineType):
        _aio.spawn(result)


def _handle(kind, value):
    if kind == "init":
        for function in _init_handlers:
            _start(function(value))
        return 0
    name = value.get("type", "") if isinstance(value, dict) else ""
    if name == "call.result":
        _aio.resolve(value)
        return 0
    for pattern, function in _event_handlers:
        if _matches(pattern, name):
            _start(function(value))
    return 0


def run(handler=None):
    """Handle events until the host stops the plugin."""
    while True:
        try:
            message = _channel.receive()
        except EOFError:
            return
        kind = message.get("t")
        if kind not in ("init", "event"):
            continue
        if handler is not None:
            status = handler(kind, message.get("v")) or 0
        else:
            try:
                status = _handle(kind, message.get("v")) or 0
            except Exception:
                try:
                    log(_traceback.format_exc())
                except Exception:
                    pass
                status = -1
            _aio.run_ready()
        _channel.send({"t": "done", "status": int(status)})
