These PEM files are **public test fixtures**, never deployment credentials.

`ca.pem` signs `localhost.pem` (valid 2020–2040) and `expired.pem`
(expired in 2021). Both leaf certificates use the deliberately published
`localhost-key.pem` P-256 private key and contain SANs `localhost` and
`127.0.0.1`, with the serverAuth extended usage. The CA private key is not kept.

Tests explicitly load this CA. Do not install it into an operating-system trust
store or use these certificates to protect real traffic. A deployment needs its
own trusted certificate chain and private key.

`sni-ca.pem` is a separate public test CA. It signs `a.example.pem` and
`b.example.pem` (valid 2020–2040), each with its own deliberately published P-256
private key and only its matching DNS SAN. `a.example.pem` also has the IPv6 IP
SAN `::1` for transport-family and IP-identity verification tests. Both are
serverAuth leaf certificates. The CA private key is not kept. These fixtures
exercise independent SNI selection, application-supplied in-memory roots and
failure isolation; they are never deployment credentials.
