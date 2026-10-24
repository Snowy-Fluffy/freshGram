"""The freshGram plugin SDK for Python.

A plugin is a Python program that the host runs inside a sandbox. It cannot
open files, sockets or processes: everything goes through the calls below, and
each call is checked against the permissions the user granted.
"""

import json as _json
import traceback as _traceback

from . import _channel
from . import _cbor

API_VERSION = 1

_output = _channel._output
call = _channel.raw_call


class FreshgramError(Exception):
    def __init__(self, code, message=""):
        super().__init__(code + (": " + message if message else ""))
        self.code = code
        self.message = message


class Denied(FreshgramError):
    pass


class Invalid(FreshgramError):
    pass


class NotFound(FreshgramError):
    pass


class QuotaExceeded(FreshgramError):
    pass


class LimitExceeded(FreshgramError):
    pass


class Unavailable(FreshgramError):
    pass


class Failed(FreshgramError):
    pass


_ERRORS = {
    "denied": Denied,
    "invalid": Invalid,
    "not_found": NotFound,
    "quota": QuotaExceeded,
    "limit": LimitExceeded,
    "unavailable": Unavailable,
}


def _request(operation, **arguments):
    response = _channel.raw_call({"op": operation, "a": arguments})
    if not isinstance(response, dict):
        raise Failed("failed", "a malformed response")
    if response.get("ok"):
        return response.get("v")
    code = response.get("e", "failed")
    raise _ERRORS.get(code, Failed)(code, response.get("m", ""))


def info():
    """The API version, the plugin id and the granted permissions."""
    return _request("meta.info")


def log(text):
    _request("log", text=str(text))


class _Storage:
    def get(self, key, default=None):
        value = _request("storage.get", key=key)
        return default if value is None else value

    def set(self, key, value):
        _request("storage.set", key=key, value=value)

    def delete(self, key):
        return _request("storage.delete", key=key)

    def keys(self, prefix="", limit=100):
        return _request("storage.list", prefix=prefix, limit=limit)

    def usage(self):
        return _request("storage.usage")


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
    addresses are refused. Timeouts are in milliseconds.
    """

    def __init__(self, socket_id):
        self.id = socket_id

    def send(self, data, timeout=10000):
        return _request("net.socket.send", id=self.id, data=data, timeout=timeout)["sent"]

    def recv(self, max_bytes=65507, timeout=1000):
        """Return (data, closed); data is empty when nothing arrived in time."""
        result = _request("net.socket.recv", id=self.id, max=max_bytes, timeout=timeout)
        return result["data"], result["closed"]

    def close(self):
        _request("net.socket.close", id=self.id)

    def __enter__(self):
        return self

    def __exit__(self, *details):
        self.close()


class _Net:
    def tcp(self, host, port, tls=False, timeout=10000):
        """Open a TCP connection (net.tcp), optionally wrapped in TLS."""
        return Socket(_request("net.tcp.connect", host=host, port=port, tls=tls, timeout=timeout)["id"])

    def udp(self, host, port, timeout=10000):
        """Open a UDP socket bound to one peer (net.udp)."""
        return Socket(_request("net.udp.connect", host=host, port=port, timeout=timeout)["id"])

    def fetch(self, url, method="GET", headers=None, body=None, timeout=10000):
        arguments = {"url": url, "method": method, "timeout": timeout}
        if headers:
            arguments["headers"] = headers
        if body is not None:
            arguments["body"] = body
        return Response(_request("net.fetch", **arguments))

    def get(self, url, **options):
        return self.fetch(url, "GET", **options)

    def post(self, url, body=None, **options):
        return self.fetch(url, "POST", body=body, **options)


class _Ui:
    def toast(self, text):
        _request("ui.toast", text=text)

    def settings_page(self, title, items):
        """Describe the plugin's settings page; the app draws it.

        Items are dicts: text, divider, toggle, input, select and button.
        """
        _request("ui.settings.set", page={"title": title, "items": items})

    def dialog(self, title, items):
        """Show a dialog (ui.dialog); clicks arrive as dialog.* events."""
        _request("ui.dialog.show", page={"title": title, "items": items})

    def close_dialog(self):
        _request("ui.dialog.close")


class _Chats:
    def list(self, limit=100):
        return _request("chats.list", limit=limit)

    def info(self, chat):
        return _request("chat.info", chat=chat)


class _Messages:
    def history(self, chat, limit=20, before=0):
        return _request("messages.history", chat=chat, limit=limit, before=before)

    def send(self, chat, text):
        _request("messages.send", chat=chat, text=text)

    def set_outgoing(self, text):
        """Replace the text of the message being sent (message.outgoing only)."""
        _request("messages.set_outgoing", text=text)


class _Plugins:
    def send(self, to, data=None):
        """Send data to another plugin (plugins.message on both sides).

        The other plugin must list this plugin (or "*") in
        contributes.messages.from; it receives a plugin.message event.
        """
        _request("plugins.send", to=to, data=data)

    def peers(self):
        """Plugins this plugin may send messages to."""
        return _request("plugins.peers")


class _Contacts:
    def list(self, limit=200):
        return _request("contacts.list", limit=limit)


class _Folders:
    def list(self):
        return _request("folders.list")


class _Settings:
    def read(self):
        return _request("settings.read")


class _Media:
    def get(self, chat, message, thumbnail=False):
        """A downloaded file of a message: kind, mime, name and data."""
        return _request("media.get", chat=chat, message=message, thumbnail=thumbnail)


class _Account:
    def info(self):
        return _request("account.info")


class _Stars:
    def balance(self):
        return _request("stars.balance")["balance"]


class _Composer:
    def draft(self, chat):
        return _request("composer.draft", chat=chat)

    def set_draft(self, chat, text):
        _request("composer.set_draft", chat=chat, text=text)


storage = _Storage()
net = _Net()
ui = _Ui()
chats = _Chats()
messages = _Messages()
account = _Account()
stars = _Stars()
composer = _Composer()
plugins = _Plugins()
contacts = _Contacts()
folders = _Folders()
settings = _Settings()
media = _Media()


def unload():
    """Stop this plugin after the current event to free its memory.

    The app starts it again on the next event, command, menu click or
    settings change it is activated by.
    """
    _request("plugin.unload")

_init_handlers = []
_event_handlers = []


def on_init(function):
    _init_handlers.append(function)
    return function


def on(pattern):
    def register(function):
        _event_handlers.append((pattern, function))
        return function
    return register


def on_menu(menu_id, surface=None):
    def register(function):
        def wrapper(event):
            if event.get("id") == menu_id and (surface is None or event.get("surface") == surface):
                function(event)
        _event_handlers.append(("menu.click", wrapper))
        return function
    return register


def on_command(name):
    def register(function):
        def wrapper(event):
            if event.get("name") == name:
                function(event)
        _event_handlers.append(("command", wrapper))
        return function
    return register


def on_plugin_message(sender=None):
    def register(function):
        def wrapper(event):
            if sender is None or event.get("from") == sender:
                function(event.get("from"), event.get("data"))
        _event_handlers.append(("plugin.message", wrapper))
        return function
    return register


def on_setting(item_id=None):
    def register(function):
        def wrapper(event):
            if item_id is None or event.get("id") == item_id:
                function(event)
        _event_handlers.append(("settings.*", wrapper))
        return function
    return register


def _matches(pattern, name):
    if pattern.endswith("*"):
        return name.startswith(pattern[:-1])
    return pattern == name


def _handle(kind, value):
    if kind == "init":
        for function in _init_handlers:
            function(value)
        return 0
    name = value.get("type", "") if isinstance(value, dict) else ""
    for pattern, function in _event_handlers:
        if _matches(pattern, name):
            function(value)
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
        _channel.send({"t": "done", "status": int(status)})
