# 8. Resolve a host and verify a TLS peer

[中文](08-dns-tls.zh-CN.md) · [Previous: UDP](07-udp.md) · [Next: WebSocket](09-websocket.md)

A network address tells you where to connect. A TLS peer name tells you which
identity to verify. Keep these separate rather than treating a resolved IP as a
replacement for the original hostname.

## 1. Start with deterministic resolution

Enable `--dns=y` and link `pace_dns`. Put this complete program in `main.cpp`:

```cpp
#include <pace/dns.hpp>
#include <pace/run.hpp>
#include <iostream>

int main() {
    try {
        pace::run([]() -> pace::task<void> {
            const auto result = co_await pace::dns::resolve("127.0.0.1");
            for (const auto& address : result.addresses)
                std::cout << address.text << '\n';
        });
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

Expect `127.0.0.1`. This numeric lookup works without an external nameserver.
Now replace it with your service's hostname. Results can contain IPv4 and IPv6;
choose a returned numeric address and construct your TCP endpoint with the
service port. Resolution itself neither connects nor automatically retries all
addresses. The helper owns its resolver through completion. For a command-line
exercise, run `xmake run tutorial_dns` or `xmake run tutorial_dns example.com` in
the repository; the latter needs working external DNS.

## 2. Configure trust before opening a TLS stream

Enable `--tls=y` and link `pace_tls` (or `pace_http1` for HTTPS). For local practice
use the public test CA and localhost certificate in the PACE checkout:

```cpp
pace::tls::context server_config(pace::tls::role::server), client_config;
server_config.use_certificate_chain("tests/certs/localhost.pem", "tests/certs/localhost-key.pem");
client_config.load_verify_file("tests/certs/ca.pem");
server_config.set_alpn_protocols({"pace-demo"});
client_config.set_alpn_protocols({"pace-demo"});
```

These paths assume the repository working directory. Use your own chain/key and
appropriate trust roots in deployment. Do not disable verification to make the
example connect. Configure before calling the helper and retain custom configurations
until its await completes; the returned stream retains the configuration state.

## 3. Upgrade the TCP connection task

In the TCP server's per-connection coroutine, replace the plain stream with:

```cpp
auto connection = co_await pace::tls::accept(std::move(accepted_tcp_stream), server_config);
co_await pace::write(connection, "Hello over verified TLS!\n");
co_await connection.shutdown();
```

On the client, make the handshake's expected identity explicit:

```cpp
auto connection = co_await pace::tls::connect(client_config, endpoint, "localhost");
auto message = co_await pace::read_exact(connection, 25);
std::cout << message;
co_await connection.shutdown();
```

The helpers return only after the handshake succeeds. `pace::read_exact` and
`pace::write` use the same spelling as TCP; here the greeting is exactly 25 bytes.
The endpoint can be `127.0.0.1`, while the certificate is checked for `localhost`.
For a real resolved host, retain that host as the peer name. TLS reads/writes now
use the same await style as TCP, but authenticated close notification matters;
a bare truncated TCP EOF is not automatically a successful TLS message ending.

## 4. Run both sides and then try a failure

From the repository root:

```sh
xmake f --tls=y --dns=y --examples=y -y
xmake -y tutorial_tls tutorial_dns
xmake run tutorial_tls
xmake run tutorial_dns
```

The [TLS companion](../../examples/tutorial_tls.cpp) retains the configurations
and listener outside the tasks, checks negotiated ALPN, validates the complete
payload and waits for both peers' shutdown before stopping. In your copy, change
the expected peer name to an unrelated host and verify that the handshake fails.
Do not turn verification off to hide that expected failure.

For HTTP, reuse this configuration with the HTTPS `http::get`/`serve` overloads
in [chapter 4](04-http.md#add-https). [DNS reference](../reference/dns.md) and
[TLS reference](../reference/tls.md) describe timeout, trust, SNI and ownership details.

For a service using system trust and no custom ALPN, the client can omit the config:
`co_await pace::tls::connect(endpoint, hostname)`. Keep the original DNS hostname
for verification. For a one-off lookup outside a running context, use
`auto addresses = pace::run(pace::dns::resolve("127.0.0.1"));`.

[Helper contracts](../reference/component-conveniences.md) and
[configuration reuse and ALPN](../reference/protocol-tuning.md#dns-and-tls) cover
the next steps after this local exercise. These helpers require PACE v0.3.1 or newer.
