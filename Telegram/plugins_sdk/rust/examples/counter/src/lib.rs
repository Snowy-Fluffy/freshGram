use freshgram::{log, storage_get, storage_set, Value};

fn init(_info: &Value) -> i32 {
    let _ = log("counter ready");
    0
}

fn event(event: &Value) -> i32 {
    let chat = event.get("chat").and_then(Value::as_i64).unwrap_or(0);
    let count = match storage_get("count") {
        Ok(value) => value.and_then(|value| value.as_i64()).unwrap_or(0) + 1,
        Err(error) => {
            let _ = log("cannot read the counter");
            return 100 + error.kind as i32 + 1;
        }
    };
    if let Err(error) = storage_set("count", count) {
        let _ = log("cannot save the counter");
        return 200 + error.kind as i32 + 1;
    }
    let _ = storage_set("last_chat", chat);
    count as i32
}

freshgram::plugin!(init = init, event = event);
