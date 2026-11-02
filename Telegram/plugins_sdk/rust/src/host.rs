use crate::cbor::Value;
use crate::rt;
use std::fmt;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ErrorKind {
    Denied,
    Invalid,
    NotFound,
    Quota,
    Limit,
    Unavailable,
    Failed,
    Cancelled,
    Transport,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Error {
    pub kind: ErrorKind,
    pub message: String,
}

impl Error {
    pub(crate) fn new(kind: ErrorKind, message: impl Into<String>) -> Error {
        Error { kind, message: message.into() }
    }

    pub(crate) fn from_response(code: &str, message: &str) -> Error {
        let kind = match code {
            "denied" => ErrorKind::Denied,
            "invalid" => ErrorKind::Invalid,
            "not_found" => ErrorKind::NotFound,
            "quota" => ErrorKind::Quota,
            "limit" => ErrorKind::Limit,
            "unavailable" => ErrorKind::Unavailable,
            "cancelled" => ErrorKind::Cancelled,
            _ => ErrorKind::Failed,
        };
        Error::new(kind, message)
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:?}: {}", self.kind, self.message)
    }
}

impl std::error::Error for Error {}

#[cfg(target_arch = "wasm32")]
#[link(wasm_import_module = "freshgram")]
extern "C" {
    #[link_name = "call"]
    fn host_call(request: *const u8, length: u32, response: *mut u32, response_length: *mut u32) -> i32;
}

#[cfg(not(target_arch = "wasm32"))]
unsafe fn host_call(_: *const u8, _: u32, _: *mut u32, _: *mut u32) -> i32 {
    -1
}

fn request(op: &str, arguments: Option<Value>, awaitable: bool) -> Vec<u8> {
    let mut entries = vec![("op".to_string(), Value::from(op))];
    if awaitable {
        entries.push(("async".to_string(), Value::Bool(true)));
    }
    if let Some(arguments) = arguments {
        entries.push(("a".to_string(), arguments));
    }
    Value::Map(entries).encode()
}

fn send(bytes: &[u8]) -> Result<Value, Error> {
    let (mut pointer, mut length) = (0u32, 0u32);
    let status = unsafe { host_call(bytes.as_ptr(), bytes.len() as u32, &mut pointer, &mut length) };
    if status != 0 || pointer == 0 {
        return Err(Error::new(ErrorKind::Transport, format!("the host refused the call ({status})")));
    }
    let decoded = {
        let answer = unsafe { std::slice::from_raw_parts(pointer as *const u8, length as usize) };
        Value::decode(answer)
    };
    rt::free(pointer, length);
    let response = decoded.map_err(|error| Error::new(ErrorKind::Transport, error.to_string()))?;
    match response.get("ok").and_then(Value::as_bool) {
        Some(true) => Ok(response.get("v").cloned().unwrap_or(Value::Null)),
        Some(false) => Err(Error::from_response(
            response.get("e").and_then(Value::as_str).unwrap_or("failed"),
            response.get("m").and_then(Value::as_str).unwrap_or(""),
        )),
        None => Err(Error::new(ErrorKind::Transport, "the answer has no status")),
    }
}

pub fn call(op: &str, arguments: Option<Value>) -> Result<Value, Error> {
    send(&request(op, arguments, false))
}

pub fn call_async(op: &str, arguments: Option<Value>) -> Result<i64, Error> {
    let answer = send(&request(op, arguments, true))?;
    answer
        .get("ticket")
        .and_then(Value::as_i64)
        .ok_or_else(|| Error::new(ErrorKind::Failed, "the answer has no ticket"))
}
