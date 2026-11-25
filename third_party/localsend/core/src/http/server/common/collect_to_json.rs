// huhucapture modification; see docs/localsend-core.md and upstream.json.
use crate::http::server::common::error::AppError;
use crate::http::limits::{CONTROL_BYTES, CONTROL_TIMEOUT};
use http_body_util::BodyExt;
use hyper::body::{Body, Incoming};
use hyper::StatusCode;
use serde::de::DeserializeOwned;

pub(crate) trait CollectToJson {
    async fn collect_to_json<T: DeserializeOwned>(self) -> Result<T, AppError>;
    async fn collect_to_json_limited<T: DeserializeOwned>(self, limit: usize) -> Result<T, AppError>;
}

impl CollectToJson for Incoming {
    async fn collect_to_json<T: DeserializeOwned>(self) -> Result<T, AppError> {
        self.collect_to_json_limited(CONTROL_BYTES).await
    }

    async fn collect_to_json_limited<T: DeserializeOwned>(mut self, limit: usize) -> Result<T, AppError> {
        if self.size_hint().lower() > limit as u64 {
            return Err(AppError::Status(StatusCode::PAYLOAD_TOO_LARGE));
        }
        let read = async {
            let mut bytes = Vec::new();
            while let Some(frame) = self.frame().await {
                let frame = frame?;
                let Ok(data) = frame.into_data() else { continue; };
                if data.len() > limit.saturating_sub(bytes.len()) {
                    return Err(AppError::Status(StatusCode::PAYLOAD_TOO_LARGE));
                }
                bytes.extend_from_slice(&data);
            }
            // Keep serde_json's recursion limit enabled. Never echo a body
            // (which can include tokens and names) into logs or error messages.
            serde_json::from_slice::<T>(&bytes)
                .map_err(|_| AppError::BadRequest("Invalid JSON body".into()))
        };
        tokio::time::timeout(CONTROL_TIMEOUT, read).await
            .map_err(|_| AppError::Status(StatusCode::REQUEST_TIMEOUT))?
    }
}
