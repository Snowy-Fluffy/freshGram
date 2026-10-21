import freshgram


@freshgram.on_init
def start(info):
    freshgram.ui.settings_page("Demo", [
        {"type": "toggle", "id": "loud", "label": "Loud", "value": False},
        {"type": "button", "id": "go", "label": "Go"},
    ])


@freshgram.on_menu("translate", "message")
def menu(event):
    freshgram.storage.set("menu", {
        "chat": event.get("chat"),
        "message": event.get("message"),
        "text": event.get("text"),
    })


@freshgram.on_command("tr")
def command(event):
    freshgram.storage.set("command", event.get("args"))


@freshgram.on_setting()
def setting(event):
    freshgram.storage.set("setting", [event["type"], event["id"], event.get("value")])


freshgram.run()
