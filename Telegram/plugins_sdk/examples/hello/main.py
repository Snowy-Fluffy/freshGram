import freshgram


@freshgram.on("message.new")
def counted(event):
    count = freshgram.storage.get("count", 0) + 1
    freshgram.storage.set("count", count)
    print("message", count, "in chat", event["chat"])


@freshgram.on_command("hello")
def hello(event):
    count = freshgram.storage.get("count", 0)
    freshgram.messages.send(event["chat"], "Hello! I have seen %d messages." % count)
    freshgram.ui.toast("Said hello")


freshgram.run()
