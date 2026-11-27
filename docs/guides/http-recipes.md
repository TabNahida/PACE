# HTTP configuration and advanced recipes

[简体中文](http-recipes.zh.md) · [Guides](README.md) · [HTTP server tutorial](../tutorial/03-http-server.md)

These recipes use the HTTP/1 module: include `<pace/http1.hpp>` and link `pace_http1`, which is built by default
(`--http1=y`). The HTTPS recipes also need `--tls=y`. The snippets run inside a coroutine or before `io.run()`;
`io` is the `pace::io_context` that owns the sockets.

## Response completion and close

By default, a complete Content-Length, chunked or bodyless response returns without
waiting for peer FIN. PACE attempts its TCP FIN / TLS `close_notify`, then releases
the connection without background cleanup. Cleanup transport errors do not replace
a completed response; context cancellation still propagates. EOF-framed responses
still wait for EOF, including authenticated TLS closure for HTTPS.

To request bounded graceful cleanup, pass `client_options` after the limits:

```cpp
pace::http::client_options closing{
    .close_policy = pace::http::client_close_policy::graceful,
    .close_timeout = std::chrono::milliseconds(25),
};
auto response = co_await pace::http::async_get(io, endpoint, "/health", {}, closing);
// For a persistent client: http::client client(std::move(socket), {}, closing);
```

`graceful` waits for the peer or this budget and propagates cleanup errors. Zero
only attempts local notification; negative values are rejected with
`std::invalid_argument`. This is not a request or read timeout. The default
`no_wait` ignores the budget (100 ms by default). Truncated bodies, incomplete
chunks and invalid trailers still fail under either policy. Server final-response
draining is separate and unchanged, including early request rejection.
See the [HTTP/1 reference](../reference/http1/README.md) for the close-policy semantics.

## Keep an explicit listener when you need control

`http::serve(io, endpoint, handler)` owns its listener and binds when the task starts.
Use the listener overload instead for immediate binding, port-zero inspection,
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

The router (`<pace/http/router.hpp>`, part of `<pace/http.hpp>`) is also the handler
accepted by HTTP/2 and HTTP/3. For one callback without routes, use
`http::make_handler(callback)`. Handlers may run on several worker contexts at once,
so keep shared mutable state synchronized. Read the
[router contract](../reference/http/router.md) before adding shared captures.

## Add HTTPS

Build with `--tls=y`. The shape of the HTTP call is the same; add a TLS
configuration and the peer name to verify:

```cpp
pace::tls::context trust; // OpenSSL default trust paths and peer verification.
trust.load_verify_file("my-ca.pem"); // Optional private CA or explicit CA bundle.
auto response = co_await pace::http::async_get(io, trust, {"127.0.0.1", 8443},
                                       "localhost", "/health");
```

`localhost` is checked against the certificate independently of the numeric TCP
address. The TLS configuration must remain alive while this lazy task uses it.
The task owns the endpoint, peer name and target values.
See the [TLS reference](../reference/tls/README.md) if your OpenSSL installation
needs an explicit CA file.

For an HTTPS server, configure a certificate chain and private key before accepting:

```cpp
pace::tls::context security(pace::tls::role::server);
security.use_certificate_chain("server-chain.pem", "server-key.pem");
security.set_alpn_protocols({"http/1.1"});
pace::tcp::listener listener(io, {"127.0.0.1", 8443});
io.spawn(pace::http::serve(listener, security, route));
io.run();
```

The certificate and key files must match and serve the names clients verify. These
objects must outlive the accept loop; declare `io` before them. Each connection
handshakes independently. HTTP/1.1 over TLS accepts ALPN `http/1.1` or no ALPN;
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
an exchange is pending. HTTP/1.1 requests on a persistent client must carry their
own `Host`. One `send` may be active at a time; HTTP/1.1 does not multiplex.
A closing or EOF-framed response, an invalid response or cancellation makes the
client unusable for another request (`is_open()` returns false); connect again.
Use `secure_client` with an already-handshaken `tls::stream` for the same
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

The same `limits` can be passed to `async_get`, `async_fetch` and the client
constructors. Defaults also cap field count, chunk line bytes, chunk count and
client informational responses; the default body limit is 1 MiB. Malformed or
ambiguous Content-Length/chunked framing is rejected and the connection is closed.
Trailers cannot redefine framing, Host, connection control or authentication fields.

These limits bound individual messages and parsing metadata. The opt-in server
options below add receive and write deadlines; global memory and connection-count
policy remain application responsibilities. Endpoint overloads do not resolve names;
the URL client resolves hostnames when the optional [DNS module](../reference/dns/README.md)
is enabled. There are no redirects, retries or decompression. HTTP/2,
[HTTP/3](../reference/http3/README.md) and [WebSocket](../reference/websocket/README.md)
have separate APIs.

## Check headers before accepting a body

A handler that returns `task<http::server_response>` (the `server_handler` type) opts into
admission, per-request body limits, deadlines and streamed responses, and can still return
an ordinary buffered `response`:

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

Admission runs after request-line, header and framing validation and before body
collection. A rejection is sent immediately, even if the peer withholds the body,
and then closes the connection. The selected limit applies to both Content-Length
and chunked input, and is selected again on each keep-alive request. The borrowed
head is valid only during the synchronous callback; one bounded socket read may
already hold a few body bytes, but the body is not awaited. Accepted requests still
reach the final handler with a complete buffered body. An exception thrown by the
admission callback yields a 500 response.

The same handler can return `streaming_response` with an owned `body_source` and
a known `content_length`. PACE calls `source->read(span<char>)` with a bounded
buffer (`server_options::response_buffer_bytes`, 64 KiB by default), waits for each
chunk to be written, then asks for another. Disconnect, cancellation or write expiry
stops further reads and releases the source. Keeping an already open file in the
source serves one snapshot without loading the whole file into memory; the
[HTTP/1 reference](../reference/http1/README.md) has a complete example.

Request and write timeouts are absolute budgets for their receive and send phases;
receiving a few bytes does not restart them, and zero disables them. They exclude
the final handler and the TLS handshake. Expiry closes the transport without a
guaranteed 408. The request budget ends on early rejection and the write budget
then covers its reply. Admission and producer calls are synchronous and must return
promptly: a deadline cannot preempt a blocked application callback.

Reference: [HTTP/1 module](../reference/http1/README.md) · [shared HTTP clients](../reference/http/clients.md).
Next: [workers and threads](../tutorial/08-workers-and-threads.md).
