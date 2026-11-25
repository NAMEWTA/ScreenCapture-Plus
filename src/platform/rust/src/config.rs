use crate::lease::MAX_FILE_BYTES;
use crate::status::{self, Error, Result};
use localsend::crypto::cert::{fingerprint_from_cert_der, verify_cert_from_pem};
use localsend::discovery::DeviceIdentity;
use localsend::http::dto_v2::RegisterDtoV2;
use localsend::http::limits::TransferLimits;
use localsend::http::state::ClientInfo;
use localsend::model::discovery::{DeviceType, ProtocolType};
use rustls::pki_types::pem::PemObject;
use rustls::pki_types::CertificateDer;
use serde::Deserialize;
use std::net::Ipv4Addr;
use std::time::Duration;

#[derive(Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Config {
    pub alias: String,
    pub addresses: Vec<String>,
    pub port: u16,
    pub cert_pem: String,
    pub private_key_pem: String,
    pub fingerprint: Option<String>,
    #[serde(default)]
    pub discovery: bool,
    pub pin: Option<String>,
    pub max_file_bytes: Option<u64>,
    pub decision_timeout_ms: Option<u64>,
    pub idle_timeout_ms: Option<u64>,
    pub commit_timeout_ms: Option<u64>,
}

pub struct Validated {
    pub addresses: Vec<Ipv4Addr>,
    pub port: u16,
    pub identity: DeviceIdentity,
    pub info: ClientInfo,
    pub discovery: bool,
    pub pin: Option<String>,
    pub limits: TransferLimits,
}

pub fn ipv4(text: &str) -> Result<Ipv4Addr> {
    let address: Ipv4Addr = text.parse().map_err(|_| Error::new(status::INVALID_ARGUMENT, "Expected a concrete IPv4 address"))?;
    if address.is_unspecified() || address.is_multicast() || address.is_broadcast() {
        return Err(Error::new(status::INVALID_ARGUMENT, "Unspecified, multicast or broadcast address is not permitted"));
    }
    Ok(address)
}

impl Config {
    pub fn validate(self) -> Result<Validated> {
        if !status::text_is_bounded(&self.alias, 256) || self.addresses.is_empty()
            || self.addresses.len() > 16 || !status::text_is_bounded(&self.cert_pem, 16384)
            || !status::text_is_bounded(&self.private_key_pem, 16384)
            || self.pin.as_ref().is_some_and(|pin| !status::text_is_bounded(pin, 128)) {
            return Err(Error::new(status::INVALID_ARGUMENT, "Invalid local identity or network scope"));
        }
        let mut addresses = self.addresses.iter().map(|text| ipv4(text)).collect::<Result<Vec<_>>>()?;
        addresses.sort();
        addresses.dedup();
        verify_cert_from_pem(self.cert_pem.clone(), None)
            .map_err(|_| Error::new(status::INVALID_ARGUMENT, "Invalid or expired LocalSend certificate"))?;
        let certificate = CertificateDer::from_pem_slice(self.cert_pem.as_bytes())
            .map_err(|_| Error::new(status::INVALID_ARGUMENT, "Invalid LocalSend PEM certificate"))?;
        let fingerprint = fingerprint_from_cert_der(&certificate);
        if self.fingerprint.is_some_and(|claimed| !claimed.eq_ignore_ascii_case(&fingerprint)) {
            return Err(Error::new(status::INVALID_ARGUMENT, "Stored identity fingerprint differs from its certificate"));
        }
        let mut limits = TransferLimits::default();
        limits.max_file_bytes = self.max_file_bytes.unwrap_or(MAX_FILE_BYTES);
        limits.max_total_bytes = limits.max_file_bytes.checked_mul(64)
            .ok_or_else(|| Error::new(status::INVALID_ARGUMENT, "File budget overflow"))?;
        if let Some(value) = self.decision_timeout_ms { limits.decision_timeout = Duration::from_millis(value); }
        if let Some(value) = self.idle_timeout_ms { limits.idle_timeout = Duration::from_millis(value); }
        if let Some(value) = self.commit_timeout_ms { limits.commit_timeout = Duration::from_millis(value); }
        limits.validate().map_err(|error| Error::new(status::INVALID_ARGUMENT, error.to_string()))?;
        Ok(Validated { addresses, port: self.port, discovery: self.discovery, pin: self.pin, limits,
            identity: DeviceIdentity { cert_pem: self.cert_pem, private_key_pem: self.private_key_pem },
            info: ClientInfo { alias: self.alias, version: "2.2".to_owned(),
                device_model: Some(format!("huhucapture {}", std::env::consts::OS)),
                device_type: Some(DeviceType::Desktop), token: fingerprint } })
    }
}

pub fn register(info: &ClientInfo, port: u16) -> RegisterDtoV2 {
    RegisterDtoV2 { alias: info.alias.clone(), version: info.version.clone(),
        device_model: info.device_model.clone(), device_type: info.device_type.clone(),
        fingerprint: info.token.clone(), port, protocol: ProtocolType::Https, download: false }
}
