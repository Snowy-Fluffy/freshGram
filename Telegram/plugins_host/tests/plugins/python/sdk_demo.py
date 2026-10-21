import freshgram


def attempt(action):
    try:
        action()
        return "ok"
    except freshgram.FreshgramError as error:
        return error.code


@freshgram.on_init
def init(info):
    freshgram.storage.set("started", True)


@freshgram.on("message.*")
def message(event):
    report = {}
    info = freshgram.info()
    report["plugin"] = info["plugin"]
    report["granted"] = sorted(info["granted"])
    count = freshgram.storage.get("count", 0) + 1
    freshgram.storage.set("count", count)
    report["count"] = count
    report["chats"] = [chat["id"] for chat in freshgram.chats.list()]
    report["has_phone"] = "phone" in freshgram.account.info()
    report["service_chat"] = attempt(lambda: freshgram.messages.history(777000))
    report["secret_chat"] = attempt(lambda: freshgram.messages.history(3))
    report["history"] = len(freshgram.messages.history(1, limit=3))
    response = freshgram.net.fetch("https://api.example.com/data", headers={"Accept": "text/plain"})
    report["fetch"] = [response.status, response.text()]
    report["evil_host"] = attempt(lambda: freshgram.net.fetch("https://evil.example.org/"))
    report["http_url"] = attempt(lambda: freshgram.net.fetch("http://api.example.com/"))
    report["ip_url"] = attempt(lambda: freshgram.net.fetch("https://127.0.0.1/"))
    report["cookie_header"] = attempt(lambda: freshgram.net.fetch("https://api.example.com/", headers={"Cookie": "a=b"}))
    report["quota"] = attempt(lambda: freshgram.storage.set("big", "x" * 5000))
    report["stars"] = freshgram.stars.balance()
    freshgram.ui.toast("demo done")
    freshgram.storage.set("report", report)


freshgram.run()
