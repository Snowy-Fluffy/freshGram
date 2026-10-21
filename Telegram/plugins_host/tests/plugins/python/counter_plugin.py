import freshgram


@freshgram.on("message.*")
def message(event):
    count = freshgram.storage.get("count", 0) + 1
    freshgram.storage.set("count", count)
    freshgram.log("count=" + str(count))


freshgram.run()
