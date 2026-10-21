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


class _Net:
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
