//! Bounded network policy shared by request parsing and transfer tasks.
//! huhucapture modification; upstream source and changes are recorded alongside
//! this vendored crate. Limits do not depend on a GUI being open.

use crate::http::dto_v2::{PrepareUploadRequestDtoV2, RegisterDtoV2};
use crate::http::server::common::error::AppError;
use crate::model::discovery::ProtocolType;
use hyper::StatusCode;
use std::time::Duration;

pub const CONTROL_BYTES: usize = 1024 * 1024;
pub const CONTROL_TIMEOUT: Duration = Duration::from_secs(30);
pub const HEADER_BYTES: usize = 64 * 1024;
pub const HEADER_COUNT: usize = 64;
pub const DATA_CHUNK_BYTES: usize = 512 * 1024;
pub const DATA_QUEUE_CHUNKS: usize = 4;

/// Policy for the bound, TLS-only v2 server. HTTP/web/v3 test APIs from upstream
/// remain available, but the application must use `start_bound_v2`.
#[derive(Clone, Debug)]
pub struct TransferLimits {
    pub control_bytes: usize,
    pub max_files: usize,
    pub max_file_bytes: u64,
    pub max_total_bytes: u64,
    pub max_name_bytes: usize,
    pub max_event_queue: usize,
    pub concurrent_uploads: usize,
    pub decision_timeout: Duration,
    pub idle_timeout: Duration,
    pub commit_timeout: Duration,
}

impl Default for TransferLimits {
    fn default() -> Self {
        Self {
            control_bytes: CONTROL_BYTES,
            max_files: 64,
            max_file_bytes: 32 * 1024 * 1024 * 1024,
            max_total_bytes: 64 * 32 * 1024 * 1024 * 1024,
            max_name_bytes: 1024,
            max_event_queue: 64,
            concurrent_uploads: 2,
            decision_timeout: Duration::from_secs(120),
            idle_timeout: Duration::from_secs(30),
            commit_timeout: Duration::from_secs(120),
        }
    }
}

impl TransferLimits {
    pub fn validate(&self) -> anyhow::Result<()> {
        anyhow::ensure!(self.control_bytes > 0 && self.control_bytes <= CONTROL_BYTES,
            "control limit must be between 1 and 1 MiB");
        anyhow::ensure!(self.max_files > 0 && self.max_files <= 64, "invalid file count limit");
        anyhow::ensure!(self.max_name_bytes > 0 && self.max_name_bytes <= 1024,
            "invalid filename limit");
        anyhow::ensure!(self.max_file_bytes > 0 && self.max_file_bytes <= 32 * 1024 * 1024 * 1024
            && self.max_total_bytes >= self.max_file_bytes && self.max_total_bytes <= 64 * self.max_file_bytes,
            "invalid file size budget");
        anyhow::ensure!(self.concurrent_uploads > 0 && self.concurrent_uploads <= 2,
            "at most two uploads may be active");
        anyhow::ensure!(self.max_event_queue >= 3 && self.max_event_queue <= 64,
            "invalid event queue budget");
        anyhow::ensure!(!self.decision_timeout.is_zero() && !self.idle_timeout.is_zero()
            && !self.commit_timeout.is_zero() && self.decision_timeout <= Duration::from_secs(300)
            && self.idle_timeout <= Duration::from_secs(120) && self.commit_timeout <= Duration::from_secs(300),
            "timeouts are outside the bounded network policy");
        Ok(())
    }

    pub(crate) fn register(&self, info: &RegisterDtoV2) -> Result<(), AppError> {
        if !crate::model::discovery::metadata_within_limits(&info.alias, &info.version,
                info.device_model.as_deref(), &info.fingerprint)
            || !is_digest(&info.fingerprint) || info.port == 0
            || info.protocol != ProtocolType::Https
        {
            return Err(AppError::BadRequest("Invalid device metadata".into()));
        }
        Ok(())
    }

    pub(crate) fn upload(&self, payload: &PrepareUploadRequestDtoV2) -> Result<(), AppError> {
        self.register(&payload.info)?;
        if payload.files.is_empty() || payload.files.len() > self.max_files {
            return Err(AppError::BadRequest("Invalid number of files".into()));
        }
        let mut total = 0_u64;
        for (id, file) in &payload.files {
            if id.is_empty() || id.len() > 128 || id != &file.id || id.contains('\0')
                || file.file_name.is_empty() || file.file_name.len() > self.max_name_bytes
                || file.file_name.chars().any(|c| matches!(c, '\0' | '/' | '\\' | ':'))
                || matches!(file.file_name.as_str(), "." | "..")
                || file.preview.as_ref().is_some_and(|preview| !preview.is_empty())
                || file.sha256.as_ref().is_some_and(|digest| !is_digest(digest))
                || file.size == 0 || file.size > self.max_file_bytes
            {
                return Err(AppError::BadRequest("Invalid file metadata".into()));
            }
            let extension = file.file_name.rsplit('.').next().unwrap_or("").to_ascii_lowercase();
            if !matches!((extension.as_str(), file.file_type.as_str()),
                ("png", "image/png") | ("gif", "image/gif") | ("mp4", "video/mp4"))
            {
                return Err(AppError::Status(StatusCode::UNSUPPORTED_MEDIA_TYPE));
            }
            if file.metadata.as_ref().is_some_and(|metadata| {
                [&metadata.modified, &metadata.accessed].iter().any(|value|
                    value.as_ref().is_some_and(|text| text.len() > 128 || text.contains('\0')))
            }) {
                return Err(AppError::BadRequest("Invalid file timestamps".into()));
            }
            total = total.checked_add(file.size)
                .filter(|total| *total <= self.max_total_bytes)
                .ok_or_else(|| AppError::Status(StatusCode::PAYLOAD_TOO_LARGE))?;
        }
        Ok(())
    }
}

pub(crate) fn is_digest(value: &str) -> bool {
    value.len() == 64 && value.bytes().all(|byte| byte.is_ascii_hexdigit())
}
