use crate::cbor::Value;
use std::sync::Once;

fn rounded(size: u32) -> usize {
    size.max(1) as usize
}

pub fn alloc(size: u32) -> u32 {
    let buffer = vec![0u8; rounded(size)].into_boxed_slice();
    Box::into_raw(buffer) as *mut u8 as usize as u32
}

pub fn free(pointer: u32, size: u32) {
    if pointer == 0 {
        return;
    }
    let slice = std::ptr::slice_from_raw_parts_mut(pointer as usize as *mut u8, rounded(size));
    drop(unsafe { Box::from_raw(slice) });
}

pub fn dispatch(pointer: u32, size: u32, handler: fn(&Value) -> i32) -> i32 {
    let bytes = unsafe { std::slice::from_raw_parts(pointer as usize as *const u8, size as usize) };
    match Value::decode(bytes) {
        Ok(value) => handler(&value),
        Err(_) => -1,
    }
}

pub fn install_panic_hook() {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| {
        std::panic::set_hook(Box::new(|info| {
            let _ = crate::api::log(&format!("panic: {info}"));
        }));
    });
}
