use crate::receive::HuhuReceive;
use crate::status::{self, Error, Result};
use serde_json::Value;

pub const RECEIVE_OFFER: u32 = 1;
pub const RECEIVE_FILE: u32 = 2;
pub const RECEIVE_END: u32 = 3;
pub const SEND_PROGRESS: u32 = 4;
pub const SEND_END: u32 = 5;
pub const DISCOVERY_CHANGED: u32 = 6;
pub const DISCOVERY_END: u32 = 7;
pub const LISTENER_ERROR: u32 = 8;
pub const STOPPED: u32 = 9;
pub const DISCOVERY_REFRESH_END: u32 = 10;
pub const EVENT_CAPACITY: usize = 64;

pub struct HuhuBuffer(pub Vec<u8>);

pub struct HuhuEvent {
    pub kind: u32,
    pub status: i32,
    pub operation: u64,
    pub json: Vec<u8>,
    pub receive: Option<Box<HuhuReceive>>,
}

impl HuhuEvent {
    pub fn new(kind: u32, status: i32, operation: u64, json: Value) -> Self {
        Self { kind, status, operation, json: json.to_string().into_bytes(), receive: None }
    }

    pub fn error(kind: u32, operation: u64, error: Error) -> Self {
        Self::new(kind, error.status, operation, serde_json::json!({
            "error": error.message, "httpStatus": error.http_status
        }))
    }
}

pub fn timeout_duration(milliseconds: u32) -> Result<std::time::Duration> {
    if milliseconds > 1000 {
        return Err(Error::new(status::INVALID_ARGUMENT, "Polling timeout exceeds 1000 ms"));
    }
    Ok(std::time::Duration::from_millis(milliseconds.into()))
}
