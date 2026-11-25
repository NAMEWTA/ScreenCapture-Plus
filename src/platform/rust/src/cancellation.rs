//! A caller-owned cancellation flag for synchronous media inspection. The
//! caller joins every reader/requester before freeing the opaque handle.

use std::sync::atomic::{AtomicBool, Ordering};

#[derive(Default)]
pub struct HuhuCancellation {
    requested: AtomicBool,
}

impl HuhuCancellation {
    pub fn request(&self) { self.requested.store(true, Ordering::Release); }
    pub fn is_requested(&self) -> bool { self.requested.load(Ordering::Acquire) }
}
