use std::fmt;

const MAX_DEPTH: usize = 32;

#[derive(Clone, Debug, PartialEq)]
pub enum Value {
    Null,
    Bool(bool),
    Int(i64),
    Float(f64),
    Bytes(Vec<u8>),
    Text(String),
    Array(Vec<Value>),
    Map(Vec<(String, Value)>),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DecodeError {
    Truncated,
    Malformed,
    Unsupported,
    TooDeep,
    IntRange,
    BadUtf8,
    KeyNotText,
    TrailingData,
}

impl fmt::Display for DecodeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "invalid CBOR: {:?}", self)
    }
}

impl std::error::Error for DecodeError {}

impl Value {
    pub fn get(&self, key: &str) -> Option<&Value> {
        match self {
            Value::Map(entries) => entries.iter().find(|(k, _)| k == key).map(|(_, v)| v),
            _ => None,
        }
    }

    pub fn as_i64(&self) -> Option<i64> {
        match self {
            Value::Int(value) => Some(*value),
            _ => None,
        }
    }

    pub fn as_f64(&self) -> Option<f64> {
        match self {
            Value::Float(value) => Some(*value),
            Value::Int(value) => Some(*value as f64),
            _ => None,
        }
    }

    pub fn as_bool(&self) -> Option<bool> {
        match self {
            Value::Bool(value) => Some(*value),
            _ => None,
        }
    }

    pub fn as_str(&self) -> Option<&str> {
        match self {
            Value::Text(value) => Some(value),
            _ => None,
        }
    }

    pub fn as_bytes(&self) -> Option<&[u8]> {
        match self {
            Value::Bytes(value) => Some(value),
            _ => None,
        }
    }

    pub fn as_array(&self) -> Option<&[Value]> {
        match self {
            Value::Array(value) => Some(value),
            _ => None,
        }
    }

    pub fn is_null(&self) -> bool {
        matches!(self, Value::Null)
    }

    pub fn encode(&self) -> Vec<u8> {
        let mut out = Vec::new();
        encode_into(self, &mut out);
        out
    }

    pub fn decode(data: &[u8]) -> Result<Value, DecodeError> {
        let mut reader = Reader { data, position: 0 };
        let value = reader.item(0)?;
        if reader.position != data.len() {
            return Err(DecodeError::TrailingData);
        }
        Ok(value)
    }
}

impl From<bool> for Value {
    fn from(value: bool) -> Value {
        Value::Bool(value)
    }
}

impl From<i64> for Value {
    fn from(value: i64) -> Value {
        Value::Int(value)
    }
}

impl From<i32> for Value {
    fn from(value: i32) -> Value {
        Value::Int(value as i64)
    }
}

impl From<u32> for Value {
    fn from(value: u32) -> Value {
        Value::Int(value as i64)
    }
}

impl From<f64> for Value {
    fn from(value: f64) -> Value {
        Value::Float(value)
    }
}

impl From<&str> for Value {
    fn from(value: &str) -> Value {
        Value::Text(value.to_string())
    }
}

impl From<String> for Value {
    fn from(value: String) -> Value {
        Value::Text(value)
    }
}

impl From<Vec<u8>> for Value {
    fn from(value: Vec<u8>) -> Value {
        Value::Bytes(value)
    }
}

impl From<Vec<Value>> for Value {
    fn from(value: Vec<Value>) -> Value {
        Value::Array(value)
    }
}

fn put_head(out: &mut Vec<u8>, major: u8, argument: u64) {
    let lead = major << 5;
    if argument < 24 {
        out.push(lead | argument as u8);
    } else if argument <= 0xFF {
        out.push(lead | 24);
        out.push(argument as u8);
    } else if argument <= 0xFFFF {
        out.push(lead | 25);
        out.extend_from_slice(&(argument as u16).to_be_bytes());
    } else if argument <= 0xFFFF_FFFF {
        out.push(lead | 26);
        out.extend_from_slice(&(argument as u32).to_be_bytes());
    } else {
        out.push(lead | 27);
        out.extend_from_slice(&argument.to_be_bytes());
    }
}

fn encode_into(value: &Value, out: &mut Vec<u8>) {
    match value {
        Value::Null => out.push(0xF6),
        Value::Bool(flag) => out.push(if *flag { 0xF5 } else { 0xF4 }),
        Value::Int(number) if *number >= 0 => put_head(out, 0, *number as u64),
        Value::Int(number) => put_head(out, 1, (-1 - *number) as u64),
        Value::Float(number) => {
            out.push(0xFB);
            out.extend_from_slice(&number.to_be_bytes());
        }
        Value::Bytes(bytes) => {
            put_head(out, 2, bytes.len() as u64);
            out.extend_from_slice(bytes);
        }
        Value::Text(text) => {
            put_head(out, 3, text.len() as u64);
            out.extend_from_slice(text.as_bytes());
        }
        Value::Array(items) => {
            put_head(out, 4, items.len() as u64);
            for item in items {
                encode_into(item, out);
            }
        }
        Value::Map(entries) => {
            put_head(out, 5, entries.len() as u64);
            for (key, item) in entries {
                put_head(out, 3, key.len() as u64);
                out.extend_from_slice(key.as_bytes());
                encode_into(item, out);
            }
        }
    }
}

struct Reader<'a> {
    data: &'a [u8],
    position: usize,
}

impl<'a> Reader<'a> {
    fn take(&mut self, count: usize) -> Result<&'a [u8], DecodeError> {
        if self.data.len() - self.position < count {
            return Err(DecodeError::Truncated);
        }
        let slice = &self.data[self.position..self.position + count];
        self.position += count;
        Ok(slice)
    }

    fn head(&mut self) -> Result<(u8, u8, u64), DecodeError> {
        let initial = self.take(1)?[0];
        let (major, info) = (initial >> 5, initial & 31);
        if info < 24 {
            return Ok((major, info, info as u64));
        } else if info > 27 {
            return Ok((major, info, 0));
        }
        let mut argument = 0u64;
        for byte in self.take(1 << (info - 24))? {
            argument = (argument << 8) | *byte as u64;
        }
        Ok((major, info, argument))
    }

    fn length(&mut self, info: u8, argument: u64) -> Result<usize, DecodeError> {
        if info > 27 {
            return Err(if info == 31 {
                DecodeError::Unsupported
            } else {
                DecodeError::Malformed
            });
        }
        usize::try_from(argument).map_err(|_| DecodeError::Truncated)
    }

    fn item(&mut self, depth: usize) -> Result<Value, DecodeError> {
        if depth > MAX_DEPTH {
            return Err(DecodeError::TooDeep);
        }
        let (major, info, argument) = self.head()?;
        if major < 2 && info > 27 {
            return Err(DecodeError::Malformed);
        }
        match major {
            0 => i64::try_from(argument)
                .map(Value::Int)
                .map_err(|_| DecodeError::IntRange),
            1 => i64::try_from(argument)
                .map(|number| Value::Int(-1 - number))
                .map_err(|_| DecodeError::IntRange),
            2 | 3 => {
                let length = self.length(info, argument)?;
                let bytes = self.take(length)?;
                if major == 2 {
                    Ok(Value::Bytes(bytes.to_vec()))
                } else {
                    String::from_utf8(bytes.to_vec())
                        .map(Value::Text)
                        .map_err(|_| DecodeError::BadUtf8)
                }
            }
            4 => {
                let count = self.length(info, argument)?;
                if count > self.data.len() - self.position {
                    return Err(DecodeError::Truncated);
                }
                let mut items = Vec::with_capacity(count);
                for _ in 0..count {
                    items.push(self.item(depth + 1)?);
                }
                Ok(Value::Array(items))
            }
            5 => {
                let count = self.length(info, argument)?;
                if count > (self.data.len() - self.position) / 2 {
                    return Err(DecodeError::Truncated);
                }
                let mut entries = Vec::with_capacity(count);
                for _ in 0..count {
                    match self.item(depth + 1)? {
                        Value::Text(key) => entries.push((key, self.item(depth + 1)?)),
                        _ => return Err(DecodeError::KeyNotText),
                    }
                }
                Ok(Value::Map(entries))
            }
            6 => Err(DecodeError::Unsupported),
            _ => match info {
                20 => Ok(Value::Bool(false)),
                21 => Ok(Value::Bool(true)),
                22 => Ok(Value::Null),
                25 => Ok(Value::Float(half_to_f64(argument as u16))),
                26 => Ok(Value::Float(f32::from_bits(argument as u32) as f64)),
                27 => Ok(Value::Float(f64::from_bits(argument))),
                _ => Err(DecodeError::Unsupported),
            },
        }
    }
}

fn half_to_f64(bits: u16) -> f64 {
    let sign = if bits & 0x8000 != 0 { -1.0 } else { 1.0 };
    let exponent = ((bits >> 10) & 31) as i32;
    let fraction = (bits & 1023) as f64;
    sign * match exponent {
        0 => fraction * 2f64.powi(-24),
        31 if fraction == 0.0 => f64::INFINITY,
        31 => f64::NAN,
        _ => (1.0 + fraction / 1024.0) * 2f64.powi(exponent - 15),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::map;

    fn round(value: Value) {
        assert_eq!(Value::decode(&value.encode()), Ok(value));
    }

    #[test]
    fn round_trips() {
        round(Value::Null);
        round(Value::Bool(true));
        for number in [0, 23, 24, 255, 256, 65535, 65536, i64::MAX, -1, -24, -25, -256, i64::MIN] {
            round(Value::Int(number));
        }
        round(Value::Float(1.5));
        round(Value::Bytes(vec![1, 2, 3]));
        round(Value::Text("привет".to_string()));
        round(map! { "a" => 1, "b" => vec![Value::Null, Value::from("x")] });
    }

    #[test]
    fn matches_rfc_examples() {
        assert_eq!(Value::Int(500).encode(), [0x19, 0x01, 0xF4]);
        assert_eq!(Value::Int(-100).encode(), [0x38, 0x63]);
        assert_eq!(Value::from("IETF").encode(), [0x64, b'I', b'E', b'T', b'F']);
        assert_eq!(Value::decode(&[0xF9, 0x3C, 0x00]), Ok(Value::Float(1.0)));
        assert_eq!(Value::decode(&[0xFA, 0x47, 0xC3, 0x50, 0x00]), Ok(Value::Float(100000.0)));
    }

    #[test]
    fn refuses_what_the_host_refuses() {
        assert_eq!(Value::decode(&[0x9F, 0xFF]), Err(DecodeError::Unsupported));
        assert_eq!(Value::decode(&[0xC1, 0x00]), Err(DecodeError::Unsupported));
        assert_eq!(Value::decode(&[0xA1, 0x01, 0x01]), Err(DecodeError::KeyNotText));
        assert_eq!(Value::decode(&[0x62, 0xC3, 0x28]), Err(DecodeError::BadUtf8));
        assert_eq!(Value::decode(&[0x1B, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF]), Err(DecodeError::IntRange));
        assert_eq!(Value::decode(&[0x19, 0x01]), Err(DecodeError::Truncated));
        assert_eq!(Value::decode(&[0x00, 0x00]), Err(DecodeError::TrailingData));
        assert_eq!(Value::decode(&[0x84, 0x00]), Err(DecodeError::Truncated));
        let deep = vec![0x81; 40];
        assert_eq!(Value::decode(&deep), Err(DecodeError::TooDeep));
    }

    #[test]
    fn finds_keys() {
        let value = map! { "chat" => 7, "text" => "hi" };
        assert_eq!(value.get("chat").and_then(Value::as_i64), Some(7));
        assert_eq!(value.get("text").and_then(Value::as_str), Some("hi"));
        assert!(value.get("none").is_none());
    }
}
