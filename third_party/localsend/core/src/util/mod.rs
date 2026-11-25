// huhucapture modification; see docs/localsend-core.md and upstream.json.
pub mod base64;
pub mod error;
pub mod filename;
#[cfg(feature = "if-addrs")]
pub mod interface;
#[cfg(any(feature = "http", feature = "multicast"))]
pub(crate) mod joined_task;
pub(crate) mod time;
