mod api;
mod cbor;
mod host;
mod rt;

pub use api::*;
pub use cbor::{DecodeError, Value};
pub use host::{call, call_async, Error, ErrorKind};

#[doc(hidden)]
pub mod __private {
    pub use crate::rt::{alloc, dispatch, free, install_panic_hook};
}

#[macro_export]
macro_rules! map {
    ($($key:expr => $value:expr),* $(,)?) => {
        $crate::Value::Map(vec![$((($key).to_string(), $crate::Value::from($value))),*])
    };
}

#[macro_export]
macro_rules! plugin {
    (event = $event:path) => {
        $crate::plugin!(init = $crate::__noop_init, event = $event);
    };
    (init = $init:path, event = $event:path) => {
        #[no_mangle]
        pub extern "C" fn fg_alloc(size: u32) -> u32 {
            $crate::__private::alloc(size)
        }

        #[no_mangle]
        pub extern "C" fn fg_free(pointer: u32, size: u32) {
            $crate::__private::free(pointer, size)
        }

        #[no_mangle]
        pub extern "C" fn fg_init(pointer: u32, size: u32) -> i32 {
            $crate::__private::install_panic_hook();
            $crate::__private::dispatch(pointer, size, $init)
        }

        #[no_mangle]
        pub extern "C" fn fg_on_event(pointer: u32, size: u32) -> i32 {
            $crate::__private::dispatch(pointer, size, $event)
        }
    };
}

#[doc(hidden)]
pub fn __noop_init(_info: &Value) -> i32 {
    0
}
