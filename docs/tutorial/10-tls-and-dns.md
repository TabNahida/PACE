# 10. TLS and DNS

[中文](10-tls-and-dns.zh.md) · [Learning path](README.md) · [Previous: orderly shutdown](09-graceful-shutdown.md)

A network address tells you where to connect. A TLS peer name tells you which identity to verify. Keep the two
apart: a resolved IP address does not replace the original hostname.

Both modules are optional. Configure the `hello` project with them before you start; xmake downloads c-ares and
OpenSSL the first time:

```sh
xmake f -m release --tests=n --examples=n --dns=y --tls=y -y
```

## Start with deterministic resolution

Save this complete program as `resolve.cpp`, with a `resolve` target that depends on `pace_dns`:

```cpp
#include <pace/dns.hpp>
#include <pace/runtime.hpp>
#include <iostream>

int main()
{
    try
    {
        pace::run(
            []() -> pace::task<void>
            {
                const auto result = co_await pace::dns::resolve("127.0.0.1");
                for (const auto& address : result.addresses)
                {
                    std::cout << address.text << '\n';
                }
            }
        );
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

Expect `127.0.0.1`. A numeric name resolves without asking a name server. Now replace it with your service's
hostname. The result can contain IPv4 and IPv6 addresses: choose one and build your TCP endpoint from it and the
service port. Resolving neither connects nor tries the addresses for you. `dns::resolve` owns its resolver until it
completes, and gives up after five seconds by default.

The companion `xmake run tutorial_dns` resolves `127.0.0.1` and prints each address with its family;
`xmake run tutorial_dns example.com` resolves a real name and needs working external DNS. Configure the PACE checkout
with `--dns=y --examples=y` first.

## Configure trust before opening a TLS stream

Link `pace_tls` (or `pace_http1` for HTTPS, which brings TLS along when it is enabled). For local practice, use the
public test CA and localhost certificate in the PACE checkout:

```cpp
pace::tls::context server_config(pace::tls::role::server), client_config;
server_config.use_certificate_chain("tests/certs/localhost.pem", "tests/certs/localhost-key.pem");
client_config.load_verify_file("tests/certs/ca.pem");
server_config.set_alpn_protocols({"pace-demo"});
client_config.set_alpn_protocols({"pace-demo"});
```

These paths are relative to the PACE checkout. `xmake run` starts a program in its build directory, so run it with
`xmake run -w third_party/PACE <target>` from the `hello` directory, or use absolute paths. In
deployment, use your own certificate chain, key and trust roots. Do not turn verification off to make the example
connect. Configure a context completely before its first use: after a stream has used it, its setters throw. Keep a
context alive until the operation that borrows it completes; the streams themselves share ownership of its state.

## Upgrade the TCP connection task

In the per-connection coroutine of a TCP server, replace the plain stream with a TLS stream:

```cpp
auto connection = co_await pace::tls::accept(std::move(accepted_tcp_stream), server_config);
co_await pace::write(connection, "Hello over verified TLS!\n");
co_await connection.shutdown();
```

On the client, name the identity the handshake must verify:

```cpp
auto connection = co_await pace::tls::connect(endpoint, "localhost", {.tls = &client_config});
auto message = co_await pace::read_exact(connection, 25);
std::cout << message;
co_await connection.shutdown();
```

Both calls return only after a successful handshake. `pace::read_exact` and `pace::write` work on TLS streams just as
on TCP; the greeting here is exactly 25 bytes. The endpoint can be `127.0.0.1` while the certificate is checked for
`localhost`. For a real host, keep the hostname you resolved as the peer name. TLS ends a stream with an
authenticated close notification: a bare TCP end of stream is not a successful end of a TLS message.

## Run both sides and then try a failure

From the PACE checkout:

```sh
xmake f -m release --tls=y --dns=y --examples=y -y
xmake -y tutorial_tls tutorial_dns
xmake run tutorial_tls
xmake run tutorial_dns
```

The companion [`tutorial_tls.cpp`](../../examples/tutorial_tls.cpp) keeps both configurations and the listener
outside the tasks, checks the negotiated ALPN protocol, reads the whole greeting, confirms that the peer's close
notification follows it, and waits for both peers to shut down before stopping. It prints
`Hello over verified TLS!`. In your own copy, change the expected peer name to an unrelated host and check that the
handshake fails. Do not turn verification off to hide that failure.

For a service with a publicly trusted certificate and no custom ALPN, the client needs no configuration:
`co_await pace::tls::connect(endpoint, hostname)` verifies with OpenSSL's default trust store (see
[chapter 2](02-http-client.md) if that store is empty). Keep the original DNS
hostname as the name to verify. For a single lookup outside a running context, use
`auto result = pace::run(pace::dns::resolve("127.0.0.1"));`.

For HTTP, give the HTTPS overloads a client context that loads your CA, for example `pace::http::get(trust, url)`
from [chapter 2](02-http-client.md); leave out the `pace-demo` ALPN, which only these two peers agree on. The HTTPS
server is in
[HTTP recipes](../guides/http-recipes.md). The [DNS reference](../reference/dns/README.md) and the
[TLS reference](../reference/tls/README.md) describe timeouts, trust, SNI and ownership;
[protocol tuning](../guides/protocol-tuning.md) covers reusing configurations and choosing ALPN.

Next: [11. WebSocket](11-websocket.md)
