pub mod ffi;

/// # Safety
/// `out` must point to `cap` writable `u64` values, or be null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rt_tui_layout_probe(out: *mut u64, cap: usize) -> usize {
    let v = ffi::layout_values();
    if !out.is_null() {
        let n = v.len().min(cap);
        // SAFETY: the caller guarantees `cap` writable slots; we write at most `cap`.
        unsafe { std::ptr::copy_nonoverlapping(v.as_ptr(), out, n) };
    }
    v.len()
}

#[unsafe(no_mangle)]
pub extern "C" fn rt_tui_main() -> i32 {
    // SAFETY: create and destroy on one thread; the handle is not used after destroy.
    let n = unsafe {
        let s = ffi::rt_session_create();
        if s.is_null() {
            return 5;
        }
        let n = ffi::rt_session_strip_count(s);
        ffi::rt_session_destroy(s);
        n
    };
    println!("rt-rig: {n} strips");
    0
}
