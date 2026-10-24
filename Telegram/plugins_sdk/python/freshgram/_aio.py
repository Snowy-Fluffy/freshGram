"""The event loop behind freshgram.aio.

The host answers an awaited call at once with a ticket and later sends a
call.result event. Coroutines run between events: after every message from
the host the loop runs whatever became ready, then the plugin reports done.
"""

import traceback as _traceback
import types as _types

from . import _channel
from . import _errors

_ready = []
_pending = {}


class Future:
    """A value that arrives later; await it inside an async function."""

    def __init__(self):
        self._done = False
        self._result = None
        self._error = None
        self._callbacks = []

    def done(self):
        return self._done

    def result(self):
        if not self._done:
            raise RuntimeError("the result is not ready")
        if self._error is not None:
            raise self._error
        return self._result

    def exception(self):
        return self._error

    def set_result(self, value):
        self._finish(value, None)

    def set_exception(self, error):
        self._finish(None, error)

    def add_done_callback(self, callback):
        if self._done:
            callback(self)
        else:
            self._callbacks.append(callback)

    def _finish(self, value, error):
        if self._done:
            return
        self._done = True
        self._result = value
        self._error = error
        callbacks, self._callbacks = self._callbacks, []
        for callback in callbacks:
            callback(self)

    def __await__(self):
        if not self._done:
            yield self
        return self.result()


class Task(Future):
    """A coroutine running on the plugin's loop."""

    def __init__(self, coroutine):
        super().__init__()
        self._coroutine = coroutine
        _ready.append((self, None))

    def _step(self, awaited):
        try:
            if awaited is not None and awaited.exception() is not None:
                waiting = self._coroutine.throw(awaited.exception())
            else:
                waiting = self._coroutine.send(None if awaited is None else awaited._result)
        except StopIteration as stop:
            self.set_result(stop.value)
            return
        except BaseException as error:
            self.set_exception(error)
            return
        if not isinstance(waiting, Future):
            failed = Future()
            failed.set_exception(TypeError("only freshgram awaitables can be awaited"))
            _ready.append((self, failed))
            return
        waiting.add_done_callback(lambda future: _ready.append((self, future)))


def ensure_future(awaitable):
    if isinstance(awaitable, Future):
        return awaitable
    if isinstance(awaitable, _types.CoroutineType):
        return Task(awaitable)
    raise TypeError("expected a coroutine or a freshgram awaitable")


def _log_failure(task):
    error = task.exception()
    if error is None:
        return
    text = "".join(_traceback.format_exception(type(error), error, error.__traceback__))
    try:
        _channel.raw_call({"op": "log", "a": {"text": text[-4000:]}})
    except Exception:
        pass


def spawn(coroutine):
    """Run a coroutine in the background; its exception, if any, is logged."""
    task = ensure_future(coroutine)
    task.add_done_callback(_log_failure)
    return task


def request(operation, arguments, then=None):
    future = Future()
    try:
        ticket = _errors.from_response(_channel.raw_call(
            {"op": operation, "a": arguments, "async": True}))["ticket"]
    except Exception as error:
        future.set_exception(error)
        return future
    _pending[ticket] = (future, then)
    return future


def resolve(event):
    entry = _pending.pop(event.get("ticket"), None)
    if entry is None:
        return
    future, then = entry
    try:
        value = _errors.from_response(event)
        future.set_result(value if then is None else then(value))
    except Exception as error:
        future.set_exception(error)


def run_ready():
    while _ready:
        task, awaited = _ready.pop(0)
        if not task.done():
            task._step(awaited)
