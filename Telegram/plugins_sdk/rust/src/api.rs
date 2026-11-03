use crate::cbor::Value;
use crate::host::{call, call_async, Error, ErrorKind};
use crate::map;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Sent {
    pub id: i64,
    pub local_id: i64,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Response {
    pub status: i64,
    pub headers: Vec<(String, String)>,
    pub body: Vec<u8>,
}

#[derive(Clone, Debug, Default)]
pub struct FetchOptions {
    pub method: Option<String>,
    pub headers: Vec<(String, String)>,
    pub body: Option<Vec<u8>>,
    pub timeout_ms: Option<i64>,
}

fn sent(answer: &Value) -> Sent {
    let field = |name| answer.get(name).and_then(Value::as_i64).unwrap_or(0);
    Sent { id: field("id"), local_id: field("local_id") }
}

fn done(answer: Result<Value, Error>) -> Result<(), Error> {
    answer.map(|_| ())
}

fn handle(answer: &Value, name: &str) -> Result<i64, Error> {
    answer
        .get(name)
        .and_then(Value::as_i64)
        .ok_or_else(|| Error { kind: ErrorKind::Failed, message: format!("the answer has no {name}") })
}

pub fn log(text: &str) -> Result<(), Error> {
    done(call("log", Some(map! { "text" => text })))
}

pub fn unload() -> Result<(), Error> {
    done(call("plugin.unload", None))
}

pub fn storage_get(key: &str) -> Result<Option<Value>, Error> {
    let answer = call("storage.get", Some(map! { "key" => key }))?;
    Ok(if answer.is_null() { None } else { Some(answer) })
}

pub fn storage_set(key: &str, value: impl Into<Value>) -> Result<(), Error> {
    done(call("storage.set", Some(map! { "key" => key, "value" => value.into() })))
}

pub fn storage_delete(key: &str) -> Result<(), Error> {
    done(call("storage.delete", Some(map! { "key" => key })))
}

pub fn storage_keys(prefix: &str, limit: i64) -> Result<Vec<String>, Error> {
    let answer = call("storage.list", Some(map! { "prefix" => prefix, "limit" => limit }))?;
    let keys = answer.as_array().unwrap_or(&[]);
    Ok(keys.iter().filter_map(|key| key.as_str().map(str::to_string)).collect())
}

pub fn sql_run(sql: &str, params: Vec<Value>, limit: i64) -> Result<Value, Error> {
    call("sql.run", Some(map! { "sql" => sql, "params" => params, "limit" => limit }))
}

pub fn silence_chats(chats: &[i64]) -> Result<(), Error> {
    let rules: Vec<Value> = chats.iter().map(|chat| map! { "chat" => *chat }).collect();
    done(call("notify.set_rules", Some(map! { "rules" => rules })))
}

pub fn send_text(chat: i64, text: &str) -> Result<Sent, Error> {
    call("messages.send", Some(map! { "chat" => chat, "text" => text })).map(|answer| sent(&answer))
}

pub fn reply_text(chat: i64, text: &str, reply_to: i64) -> Result<Sent, Error> {
    let arguments = map! { "chat" => chat, "text" => text, "reply_to" => reply_to };
    call("messages.send", Some(arguments)).map(|answer| sent(&answer))
}

pub fn send_media(chat: i64, data: Vec<u8>, name: &str, caption: &str, kind: &str) -> Result<Sent, Error> {
    let arguments = map! {
        "chat" => chat,
        "data" => data,
        "name" => name,
        "caption" => caption,
        "kind" => kind,
    };
    call("messages.send_media", Some(arguments)).map(|answer| sent(&answer))
}

pub fn edit_text(chat: i64, message: i64, text: &str) -> Result<(), Error> {
    done(call("messages.edit", Some(map! { "chat" => chat, "id" => message, "text" => text })))
}

pub fn set_display(chat: i64, message: i64, text: &str) -> Result<(), Error> {
    done(call("messages.set_display", Some(map! { "chat" => chat, "id" => message, "text" => text })))
}

pub fn reset_display(chat: i64, message: i64) -> Result<(), Error> {
    done(call("messages.set_display", Some(map! { "chat" => chat, "id" => message })))
}

pub fn edit_media(chat: i64, message: i64, data: Vec<u8>, name: &str, caption: &str, kind: &str) -> Result<(), Error> {
    let arguments = map! {
        "chat" => chat,
        "id" => message,
        "data" => data,
        "name" => name,
        "caption" => caption,
        "kind" => kind,
    };
    done(call("messages.edit_media", Some(arguments)))
}

pub fn react(chat: i64, message: i64, emoji: &str) -> Result<(), Error> {
    done(call("messages.react", Some(map! { "chat" => chat, "id" => message, "emoji" => emoji })))
}

pub fn set_outgoing(text: &str) -> Result<(), Error> {
    done(call("messages.set_outgoing", Some(map! { "text" => text })))
}

pub fn drop_outgoing(reason: &str) -> Result<(), Error> {
    let arguments = if reason.is_empty() { None } else { Some(map! { "reason" => reason }) };
    done(call("messages.drop_outgoing", arguments))
}

pub fn silence_incoming() -> Result<(), Error> {
    done(call("messages.silence_incoming", None))
}

/// A file of the send being made, as a map with kind, mime, name and data
/// (message.outgoing only; needs media.read_outgoing). `index` is the position in the event's media list.
pub fn outgoing_file(index: i64, thumbnail: bool) -> Result<Value, Error> {
    call("messages.outgoing_file", Some(map! { "index" => index, "thumbnail" => thumbnail }))
}

/// The file of a message, as a map with kind, mime, name and data (up to 100 MiB).
/// With `download` a file that is not on this device is downloaded first; that needs media.download.
pub fn media_get(chat: i64, message: i64, thumbnail: bool, download: bool) -> Result<Value, Error> {
    call(
        "media.get",
        Some(map! { "chat" => chat, "message" => message, "thumbnail" => thumbnail, "download" => download }),
    )
}

/// Messages with media, newest first. `kind` is photo, video, photo_video, file, voice, round, audio, gif or any.
pub fn media_get_with_progress(chat: i64, message: i64, thumbnail: bool) -> Result<Value, Error> {
    call(
        "media.get",
        Some(map! { "chat" => chat, "message" => message, "thumbnail" => thumbnail, "download" => true, "progress" => true }),
    )
}

pub fn media_list(chat: i64, kind: &str, limit: i64, before: i64) -> Result<Value, Error> {
    call("media.list", Some(map! { "chat" => chat, "kind" => kind, "limit" => limit, "before" => before }))
}

pub fn chat_info(chat: i64) -> Result<Value, Error> {
    call("chat.info", Some(map! { "chat" => chat }))
}

pub fn history(chat: i64, limit: i64, before: i64) -> Result<Value, Error> {
    call("messages.history", Some(map! { "chat" => chat, "limit" => limit, "before" => before }))
}

pub fn toast(text: &str) -> Result<(), Error> {
    done(call("ui.toast", Some(map! { "text" => text })))
}

pub fn notify(title: &str, text: &str) -> Result<(), Error> {
    done(call("notify.show", Some(map! { "title" => title, "text" => text })))
}

pub fn timer_set(id: &str, delay_ms: i64, repeat: bool) -> Result<(), Error> {
    done(call("timers.set", Some(map! { "id" => id, "delay" => delay_ms, "repeat" => repeat })))
}

pub fn timer_clear(id: &str) -> Result<(), Error> {
    done(call("timers.clear", Some(map! { "id" => id })))
}

pub fn sleep_async(ms: i64) -> Result<i64, Error> {
    call_async("time.sleep", Some(map! { "ms" => ms }))
}

pub fn fetch(url: &str, options: FetchOptions) -> Result<Response, Error> {
    let mut entries = vec![("url".to_string(), Value::from(url))];
    if let Some(method) = options.method {
        entries.push(("method".to_string(), Value::from(method)));
    }
    if !options.headers.is_empty() {
        let headers = options.headers.into_iter().map(|(name, value)| (name, Value::from(value)));
        entries.push(("headers".to_string(), Value::Map(headers.collect())));
    }
    if let Some(body) = options.body {
        entries.push(("body".to_string(), Value::Bytes(body)));
    }
    if let Some(timeout) = options.timeout_ms {
        entries.push(("timeout".to_string(), Value::Int(timeout)));
    }
    let answer = call("net.fetch", Some(Value::Map(entries)))?;
    let pair = |item: &Value| {
        let pair = item.as_array()?;
        Some((pair.first()?.as_str()?.to_string(), pair.get(1)?.as_str()?.to_string()))
    };
    Ok(Response {
        status: handle(&answer, "status")?,
        headers: answer.get("header_list").and_then(Value::as_array).unwrap_or(&[]).iter().filter_map(pair).collect(),
        body: answer.get("body").and_then(Value::as_bytes).unwrap_or(&[]).to_vec(),
    })
}

pub fn tcp_connect(host: &str, port: i64, tls: bool) -> Result<i64, Error> {
    let arguments = map! { "host" => host, "port" => port, "tls" => tls };
    handle(&call("net.tcp.connect", Some(arguments))?, "id")
}

pub fn udp_connect(host: &str, port: i64) -> Result<i64, Error> {
    handle(&call("net.udp.connect", Some(map! { "host" => host, "port" => port }))?, "id")
}

pub fn socket_send(id: i64, data: Vec<u8>) -> Result<(), Error> {
    done(call("net.socket.send", Some(map! { "id" => id, "data" => data })))
}

pub fn socket_recv(id: i64, max: i64, timeout_ms: i64) -> Result<(Vec<u8>, bool), Error> {
    let answer = call("net.socket.recv", Some(map! { "id" => id, "max" => max, "timeout" => timeout_ms }))?;
    let data = answer.get("data").and_then(Value::as_bytes).unwrap_or(&[]).to_vec();
    Ok((data, answer.get("closed").and_then(Value::as_bool).unwrap_or(false)))
}

pub fn socket_close(id: i64) -> Result<(), Error> {
    done(call("net.socket.close", Some(map! { "id" => id })))
}

pub fn plugin_send(to: &str, data: impl Into<Value>) -> Result<(), Error> {
    done(call("plugins.send", Some(map! { "to" => to, "data" => data.into() })))
}

pub fn translate_reply(ticket: i64, items: &[&str]) -> Result<(), Error> {
    let items: Vec<Value> = items.iter().map(|item| Value::from(*item)).collect();
    done(call("translate.reply", Some(map! { "ticket" => ticket, "items" => items })))
}

pub fn translate_error(ticket: i64, error: &str) -> Result<(), Error> {
    done(call("translate.reply", Some(map! { "ticket" => ticket, "error" => error })))
}

pub fn ai_reply(ticket: i64, text: &str) -> Result<(), Error> {
    done(call("ai.reply", Some(map! { "ticket" => ticket, "text" => text })))
}

pub fn ai_error(ticket: i64, error: &str) -> Result<(), Error> {
    done(call("ai.reply", Some(map! { "ticket" => ticket, "error" => error })))
}
