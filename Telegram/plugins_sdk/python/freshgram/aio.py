"""Awaitable versions of the freshgram calls.

    from freshgram import aio

    @freshgram.on("command")
    async def weather(event):
        response = await aio.net.get("https://example.org/weather")
        await aio.ui.toast(response.text()[:100])

Each call is sent to the app at once and answered later, so the plugin keeps
handling other events while it waits. The calls, permissions and limits are
the same as in freshgram; at most 32 calls may wait at a time. Only
freshgram awaitables can be awaited: asyncio does not run here.
"""

from . import _aio
from . import (
    _Account,
    _Chats,
    _Clipboard,
    _Composer,
    _Contacts,
    _Files,
    _Folders,
    _Media,
    _Messages,
    _Net,
    _Notify,
    _Plugins,
    _Settings,
    _Stars,
    _Storage,
    _Timers,
    _Ui,
    _Users,
    _async_call,
)

Future = _aio.Future
Task = _aio.Task
spawn = _aio.spawn

storage = _Storage(_async_call)
net = _Net(_async_call)
ui = _Ui(_async_call)
chats = _Chats(_async_call)
messages = _Messages(_async_call)
account = _Account(_async_call)
stars = _Stars(_async_call)
composer = _Composer(_async_call)
plugins = _Plugins(_async_call)
contacts = _Contacts(_async_call)
folders = _Folders(_async_call)
settings = _Settings(_async_call)
media = _Media(_async_call)
users = _Users(_async_call)
notify = _Notify(_async_call)
clipboard = _Clipboard(_async_call)
files = _Files(_async_call)
timers = _Timers(_async_call)


def sleep(seconds):
    """Wait without blocking the plugin, up to 60 seconds."""
    return _aio.request("time.sleep", {"ms": max(0, int(seconds * 1000))})


def gather(*awaitables):
    """Wait for all of them; the result is the list of their results.

    The first failure is raised; the other calls keep running.
    """
    futures = [_aio.ensure_future(item) for item in awaitables]
    result = _aio.Future()
    left = [len(futures)]
    if not futures:
        result.set_result([])
        return result

    def finished(future):
        if result.done():
            return
        if future.exception() is not None:
            result.set_exception(future.exception())
            return
        left[0] -= 1
        if left[0] == 0:
            result.set_result([item.result() for item in futures])

    for future in futures:
        future.add_done_callback(finished)
    return result


def wait_for(awaitable, timeout):
    """Wait at most timeout seconds (up to 60); raise TimeoutError after that.

    The call itself is not cancelled: its result is dropped.
    """
    future = _aio.ensure_future(awaitable)
    result = _aio.Future()

    def finished(item):
        if result.done():
            return
        if item.exception() is not None:
            result.set_exception(item.exception())
        else:
            result.set_result(item.result())

    def expired(item):
        if item.exception() is None and not result.done():
            result.set_exception(TimeoutError("the call did not finish in time"))

    future.add_done_callback(finished)
    if not result.done():
        sleep(min(timeout, 60)).add_done_callback(expired)
    return result
