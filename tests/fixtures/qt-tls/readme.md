# Public loopback TLS fixture

`localhost-test-cert.pem` and `localhost-test-key.pem` are an intentionally
public, generated test pair. The key is not a deployment credential and must
never identify a production service or enter a user trust store. The certificate
is self-signed, covers `localhost` / `127.0.0.1`, and is valid from 2020 to 2100 so
that the test exercises trust rejection rather than routine date expiry.

`tests/qt/package_runtime_probe.cpp` binds a real ephemeral loopback TLS server
with these files. The client uses normal peer verification and never calls
`ignoreSslErrors`. A successful test means the actual deployed backend rejected
the certificate. It does not make an external connection or install a CA.

The pair was generated with Python cryptography using RSA-2048, SHA256,
`BasicConstraints(ca=False)`, SubjectAlternativeName for the above two names,
and TraditionalOpenSSL PEM key encoding. The complete fixture bytes are tracked
here; no test depends on generating an identical random key again.
