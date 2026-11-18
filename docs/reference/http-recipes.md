# HTTP configuration and advanced recipes

[Guided HTTP tutorial](../tutorial/04-http.md) · [中文](http-recipes.zh-CN.md)

## Response completion and close

By default, a complete Content-Length, chunked or bodyless response returns without
waiting for peer FIN. PACE attempts its TCP FIN / TLS `close_notify`, then releases
the connection without background cleanup. Cleanup transport errors do not replace
a completed response; context cancellation still propagates. EOF-framed responses
still wait for EOF, including authenticated TLS closure for HTTPS.

To request bounded graceful cleanup, pass options after the existing limits:

```cpp
pace::http::client_options closing{
    .close_policy = pace::http::client_close_policy::graceful,
    .close_timeout = std::chrono::milliseconds(25),
};
auto response = co_await pace::http::async_get(io, endpoint, "/health", {}, closing);
// For a persistent client: http::client client(std::move(socket), {}, closing);
```

`graceful` waits for the peer or this budget and propagates cleanup errors. Zero
only attempts local notification; negative values are rejected. This is not a
request/read timeout. The default `no_wait` ignores the budget. Server final
response draining remains unchanged, including early request rejection.
See [close-policy semantics](../reference/http.md#final-response-close-policy).

## Keep an explicit listener when you need control

Use the original listener overload for immediate binding, port-zero inspection,
or stopping acceptance separately. Declare the listener outside the tasks and
keep it alive through `io.run()`:

```cpp
pace::io_context io;
pace::tcp::listener listener(io, {"127.0.0.1", 0});
pace::http::router app;
app.get("/health", [](pace::http::request) { return pace::http::response{.body = "ok\n"}; });
io.spawn(pace::http::serve(listener, app));
// Submit a client/control task that eventually calls io.stop().
io.run();
```

The router also converts to the shared handler accepted by HTTP/2 and HTTP/3.
For one synchronous callback without routes, use `http::make_handler(callback)`.
Keep mutable state synchronized across worker threads. Read the
[router contract](../reference/http-router.md) before adding shared captures.

## Add HTTPS

Build optional OpenSSL support with `--tls=y`. The shape of the HTTP call is the
same; add a TLS configuration and verified peer name:

```cpp
pace::tls::context trust; // OpenSSL default trust paths; verification is required.
trust.load_verify_file("my-ca.pem"); // Optional private CA or explicit CA bundle.
auto response = co_await pace::http::async_get(io, trust, {"127.0.0.1", 8443},
                                       "localhost", "/health");
```

`localhost` is checked against the certificate independently of the numeric TCP
address. The TLS configuration must remain alive while this lazy helper uses it.
The task owns endpoint, peer name and target values. There is no insecure mode.
See [TLS trust setup](../reference/tls.md#configuration) if your OpenSSL installation
needs an explicit CA file.

For an HTTPS server, configure a certificate/private key before accepting:

```cpp
pace::tls::context security(pace::tls::role::server);
security.use_certificate_chain("server-chain.pem", "server-key.pem");
security.set_alpn_protocols({"http/1.1"});
pace::tcp::listener listener(io, {"127.0.0.1", 8443});
io.spawn(pace::http::serve(listener, security, route));
io.run();
```

The certificate/key files must match and serve the names clients verify. These
objects must outlive the accept loop; declare `io` before them. Each connection
handshakes independently. Ordinary HTTP/1.1 TLS accepts ALPN `http/1.1` or no ALPN;
use the separate HTTP/2 API for `h2`. The committed `tests/certs` keys are public
fixtures for tests only, not deployment credentials.

## Reuse a connection

For several exchanges, construct a persistent client from a connected transport:

```cpp
pace::tcp::stream socket(io);
co_await socket.connect(endpoint);
pace::http::client client(std::move(socket));
pace::http::request request;
request.method = "GET";
request.target = "/health";
request.headers.push_back({"Host", "localhost"});
auto first = co_await client.send(request);
auto second = co_await client.send(std::move(request));
client.close();
```

The client owns the socket and parsing buffer. Keep it at a stable address while
an exchange is pending. One `send` may be active at a time; no concurrent request
multiplexing occurs. A closing/EOF-framed response makes it unusable for another
request. Use `secure_client` with an already-handshaken `tls::stream` for the same
pattern over HTTPS. Do not interleave raw transport reads with HTTP operations.

## Set practical bounds

```cpp
pace::http::limits bounds;
bounds.max_header_bytes = 8192;
bounds.max_body_bytes = 65536;
bounds.max_requests_per_connection = 100;
bounds.max_trailer_bytes = 2048;
io.spawn(pace::http::serve(listener, route, bounds));
```

The same `limits` can be passed to clients. Defaults also cap field count, chunk
line bytes, chunk count and client informational responses. Malformed or ambiguous
Content-Length/chunked framing is rejected and the connection is closed. Trailers
cannot redefine framing, Host, connection control or authentication fields.

These limits bound individual messages and parsing metadata. The opt-in server
options below add receive/write deadlines; global memory and connection-count
policy remain application responsibilities. Endpoint overloads do not resolve names. The URL client can use the optional DNS
module; redirects, retries and decompression are absent. HTTP/2,
[HTTP/3](../reference/http3.md) and WebSocket have separate APIs. Hostnames can
be resolved explicitly with the optional [DNS extension](../reference/dns.md).

## Check headers before accepting a body

Use the new `server_handler` return type to opt in while retaining ordinary
buffered responses:

```cpp
pace::task<pace::http::server_response> admitted_route(pace::http::request request) {
    co_return pace::http::response{.body = request.body};
}

pace::http::server_options options;
options.admit = [](const pace::http::request_head& head) {
    if (head.header("Authorization") != "Bearer example")
        return pace::http::admission::reject(pace::http::response{.status = 401});
    return pace::http::admission::accept(head.target == "/import" ? 1024 * 1024 : 4096);
};
options.request_timeout = std::chrono::seconds(10);
options.write_timeout = std::chrono::seconds(30);
io.spawn(pace::http::serve(listener, admitted_route, options));
```

Admission runs after framing validation and before body collection. A rejection
is sent immediately, even if the peer withholds the body, and then closes the
connection. The selected limit applies to both Content-Length and chunked input,
and is selected again on each keep-alive request. The borrowed head is valid only
during the synchronous callback. Accepted requests still reach the final handler
with a complete buffered body.

The same handler can return `streaming_response` with an owned `body_source` and
a known `content_length`. PACE calls `source->read(span<char>)` with a bounded
buffer, waits for each chunk to be written, then asks for another. Disconnect,
cancellation or write expiry releases the source. See the
[open-file producer example](../reference/http.md#bounded-response-producers)
for serving a snapshot without loading the whole file into memory.

Request/write timeouts are absolute budgets for their receive/send phases;
they exclude the final handler and TLS handshake. Expiry closes the transport.
The request budget ends on early rejection and the write budget then covers its
reply. Producer/admission calls are synchronous and must return promptly.
[Deadline details](../reference/http.md#server-deadlines) describe these boundaries.

Reference: [HTTP/1.1 and HTTPS API](../reference/http.md).
Next: [execution and worker selection](../tutorial/05-execution.md).
