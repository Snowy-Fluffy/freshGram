import freshgram

results = {}


def attempt(name, action):
    try:
        action()
        results[name] = "ok"
    except freshgram.Denied:
        results[name] = "denied"
    except freshgram.FreshgramError as error:
        results[name] = error.code


@freshgram.on("message.*")
def message(event):
    attempt("storage", lambda: freshgram.storage.set("k", 1))
    attempt("net", lambda: freshgram.net.get("https://api.example.com/"))
    attempt("chats", lambda: freshgram.chats.list())
    attempt("account", lambda: freshgram.account.info())
    attempt("stars", lambda: freshgram.stars.balance())
    attempt("toast", lambda: freshgram.ui.toast("x"))
    attempt("send", lambda: freshgram.messages.send(1, "x"))
    freshgram.log("results=" + repr(sorted(results.items())))


freshgram.run()
