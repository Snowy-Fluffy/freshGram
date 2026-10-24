import freshgram
from freshgram import aio


@freshgram.on("message.new")
async def handle(event):
    text = event.get("text")
    if text == "slow":
        response, _ = await aio.gather(aio.net.get("https://api.example.com/slow"), aio.sleep(0.3))
        freshgram.storage.set("slow", response.status)
        freshgram.log("slow done")
    elif text == "sync":
        freshgram.storage.set("sync", freshgram.net.get("https://api.example.com/sync").status)
        freshgram.log("sync done")
    else:
        freshgram.storage.set("fast", freshgram.storage.get("fast", 0) + 1)
        freshgram.log("fast")


freshgram.run()
