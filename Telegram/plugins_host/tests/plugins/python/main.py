import freshgram


def handle(kind, value):
    response = freshgram.call({"op": "echo", "kind": kind, "value": value})
    print("response:", response)
    if value.get("crash"):
        raise RuntimeError("asked to crash")
    return len(value)


freshgram.run(handle)
