use localsend::http::client::ClientError;
use localsend::http::server::common::verified_upload::UploadFailure;

pub const OK: i32 = 0;
pub const TIMEOUT: i32 = 1;
pub const END: i32 = 2;
pub const PARTIAL: i32 = 3;
pub const INVALID_ARGUMENT: i32 = -1;
pub const INVALID_STATE: i32 = -2;
pub const IO: i32 = -3;
pub const CANCELLED: i32 = -4;
pub const BUSY: i32 = -5;
pub const PROTOCOL: i32 = -6;
pub const CHECKSUM_MISMATCH: i32 = -7;
pub const LENGTH_MISMATCH: i32 = -8;
pub const INVALID_MEDIA: i32 = -9;
pub const DURABILITY_UNKNOWN: i32 = -10;
pub const CLOSED: i32 = -11;
pub const PANIC: i32 = -12;
pub const LIMIT_EXCEEDED: i32 = -13;

#[derive(Debug)]
pub struct Error {
    pub status: i32,
    pub message: String,
    pub http_status: Option<u16>,
}

pub type Result<T> = std::result::Result<T, Error>;

impl Error {
    pub fn new(status: i32, message: impl Into<String>) -> Self {
        let mut message = message.into();
        if message.len() > 1024 {
            let mut end = 1024;
            while !message.is_char_boundary(end) { end -= 1; }
            message.truncate(end);
        }
        Self { status, message, http_status: None }
    }
}

impl From<std::io::Error> for Error {
    fn from(error: std::io::Error) -> Self { Self::new(IO, error.to_string()) }
}

impl From<serde_json::Error> for Error {
    fn from(error: serde_json::Error) -> Self { Self::new(INVALID_ARGUMENT, error.to_string()) }
}

impl From<ClientError> for Error {
    fn from(error: ClientError) -> Self {
        let (status, http_status) = match &error {
            ClientError::Cancelled => (CANCELLED, None),
            ClientError::StatusCode(value) => (match value.status {
                403 => CANCELLED,
                409 | 429 => BUSY,
                413 => LIMIT_EXCEEDED,
                415 => INVALID_MEDIA,
                422 => CHECKSUM_MISMATCH,
                _ => PROTOCOL,
            }, Some(value.status)),
            ClientError::Io(_) => (IO, None),
            _ => (PROTOCOL, None),
        };
        // reqwest URLs contain upload tokens. Expose their failure class, not
        // a URL containing session credentials, in a user-facing event.
        let message = match &error {
            ClientError::Reqwest(value) => {
                if value.is_timeout() { "LocalSend request timed out" }
                else if value.is_connect() { "LocalSend TLS connection failed" }
                else if value.is_body() { "LocalSend upload stream failed" }
                else { "LocalSend request failed" }.to_owned()
            }
            _ => error.to_string(),
        };
        let mut result = Self::new(status, message);
        result.http_status = http_status;
        result
    }
}

pub fn failure_status(failure: UploadFailure) -> i32 {
    match failure {
        UploadFailure::Cancelled => CANCELLED,
        UploadFailure::Io => IO,
        UploadFailure::LengthMismatch => LENGTH_MISMATCH,
        UploadFailure::ChecksumMismatch => CHECKSUM_MISMATCH,
        UploadFailure::InvalidMedia => INVALID_MEDIA,
        UploadFailure::LimitExceeded => LIMIT_EXCEEDED,
        UploadFailure::DurabilityUnknown => DURABILITY_UNKNOWN,
    }
}

pub fn status_failure(status: i32) -> Result<UploadFailure> {
    match status {
        CANCELLED => Ok(UploadFailure::Cancelled),
        IO => Ok(UploadFailure::Io),
        LENGTH_MISMATCH => Ok(UploadFailure::LengthMismatch),
        CHECKSUM_MISMATCH => Ok(UploadFailure::ChecksumMismatch),
        INVALID_MEDIA => Ok(UploadFailure::InvalidMedia),
        LIMIT_EXCEEDED => Ok(UploadFailure::LimitExceeded),
        DURABILITY_UNKNOWN => Ok(UploadFailure::DurabilityUnknown),
        _ => Err(Error::new(INVALID_ARGUMENT, "Invalid receive completion status")),
    }
}

pub fn text_is_bounded(text: &str, maximum: usize) -> bool {
    !text.is_empty() && text.len() <= maximum && !text.contains('\0')
}

pub fn digest_is_valid(text: &str) -> bool {
    text.len() == 64 && text.bytes().all(|byte| byte.is_ascii_hexdigit())
}
