These PEM files are **public test fixtures**, never deployment credentials.

`ca.pem` signs `localhost.pem` (valid 2020–2040) and `expired.pem`
(expired in 2021). Both leaf certificates use the deliberately published
`localhost-key.pem` P-256 private key and contain SANs `localhost` and
`127.0.0.1`, with the serverAuth extended usage. The CA private key is not kept.

Tests explicitly load this CA. Do not install it into an operating-system trust
store or use these certificates to protect real traffic. A deployment needs its
own trusted certificate chain and private key.
