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
        pace::run([](pace::io_context& io) -> pace::task<void> {
            pace::dns::resolver resolver(io);
            const auto result = co_await resolver.resolve("127.0.0.1");
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
addresses. Keep the resolver alive until `resolve` finishes. For a command-line
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
example connect. Configure before first stream use: configurations freeze, and
must remain alive while their streams use them.

## 3. Upgrade the TCP connection task

In the TCP server's per-connection coroutine, replace the plain stream with:

```cpp
pace::tls::stream connection(std::move(accepted_tcp_stream), server_config);
co_await connection.handshake();
co_await connection.write_all("Hello over verified TLS!\n");
co_await connection.shutdown();
```

On the client, make the handshake's expected identity explicit:

```cpp
pace::tls::stream connection(io, client_config);
co_await connection.connect(endpoint, "localhost");
```

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
