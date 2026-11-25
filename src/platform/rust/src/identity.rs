//! Validation reuses the pinned LocalSend certificate rules and rustls's
//! private-key loader/SPKI comparison. It does not construct a runtime or bind
//! a socket. Parser errors never contain the supplied private identity.
use crate::status::{self, Error, Result};
use localsend::crypto::cert::{fingerprint_from_cert_der, verify_cert_from_pem};
use rustls::pki_types::pem::PemObject;
use rustls::pki_types::{CertificateDer, PrivateKeyDer};
use serde::Deserialize;

#[derive(Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Identity {
    pub cert_pem: String,
    pub private_key_pem: String,
    pub fingerprint: String,
}

pub fn validate(cert_pem: &str, private_key_pem: &str, expected: Option<&str>) -> Result<String> {
    if !status::text_is_bounded(cert_pem, 16384)
        || !status::text_is_bounded(private_key_pem, 16384)
        || expected.is_some_and(|value| !status::digest_is_valid(value)) {
        return Err(Error::new(status::INVALID_ARGUMENT, "Invalid stored identity fields"));
    }
    verify_cert_from_pem(cert_pem.to_owned(), None)
        .map_err(|_| Error::new(status::INVALID_ARGUMENT, "Invalid or expired LocalSend certificate"))?;
    let certificate = CertificateDer::from_pem_slice(cert_pem.as_bytes())
        .map_err(|_| Error::new(status::INVALID_ARGUMENT, "Invalid stored certificate"))?;
    let key = PrivateKeyDer::from_pem_slice(private_key_pem.as_bytes())
        .map_err(|_| Error::new(status::INVALID_ARGUMENT, "Invalid stored private key"))?;
    let fingerprint = fingerprint_from_cert_der(&certificate);
    if expected.is_some_and(|value| !value.eq_ignore_ascii_case(&fingerprint)) {
        return Err(Error::new(status::INVALID_ARGUMENT, "Stored certificate fingerprint differs"));
    }
    let pair = rustls::sign::CertifiedKey::from_der(vec![certificate], key,
        &rustls::crypto::ring::default_provider())
        .map_err(|_| Error::new(status::INVALID_ARGUMENT, "Invalid stored certificate and key pair"))?;
    // from_der permits an unknown consistency result for some providers. The
    // native RSA identity must actually match, so require the explicit check.
    pair.keys_match()
        .map_err(|_| Error::new(status::INVALID_ARGUMENT, "Stored certificate and key do not match"))?;
    if pair.key.algorithm() != rustls::SignatureAlgorithm::RSA {
        return Err(Error::new(status::INVALID_ARGUMENT, "Stored identity is not a LocalSend RSA key"));
    }
    Ok(fingerprint)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn official_identity_validates_and_damaged_or_mismatched_pair_is_rejected() {
        let first = localsend::crypto::cert::generate_self_signed().unwrap();
        let second = localsend::crypto::cert::generate_self_signed().unwrap();
        assert_eq!(validate(&first.certificate_pem, &first.private_key_pem,
            Some(&first.fingerprint)).unwrap(), first.fingerprint);
        assert!(validate(&first.certificate_pem, &second.private_key_pem, None).is_err());
        assert!(validate(&first.certificate_pem, &first.private_key_pem,
            Some(&second.fingerprint)).is_err());
        assert!(validate(&first.certificate_pem, "damaged", None).is_err());
    }
}
