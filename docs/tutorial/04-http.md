# 4. HTTP and HTTPS in a few calls

[中文教程](04-http.zh-CN.md)

Run the self-contained example:

```sh
xmake f -m release --examples=y --tests=y -y
xmake -y
xmake run tutorial_http_client
```

[`tutorial_http_client.cpp`](../../examples/tutorial_http_client.cpp) starts a
loopback HTTP server, sends a GET, verifies its chunked body and trailer, then
stops. It prints `Hello from /PACE`. No external server is needed.

## The client

```cpp
pace::task<void> use_http(pace::io_context& io, pace::tcp::endpoint endpoint) {
    auto response = co_await pace::http::get(io, endpoint, "/health");
    std::cout << response.status << " " << response.body << '\n';
    io.stop();
}
```

`get` connects, generates Host, handles response framing and closes. Its endpoint
is numeric IPv4 or IPv6; a DNS lookup is not performed. IPv6 addresses such as
`::1` remain unbracketed in the endpoint; generated Host authorities are bracketed. A 404 or 500 is a normal response
whose `status` you inspect. Protocol/transport failures throw.

To POST, use `fetch` with an owned message:

```cpp
pace::http::request request;
request.method = "POST";
request.target = "/echo";
request.headers.push_back({"Content-Type", "text/plain"});
request.body = "hello";
auto response = co_await pace::http::fetch(io, endpoint, std::move(request));
```

The helper fills Host when absent. Do not manually set Content-Length,
Transfer-Encoding or Connection. Set `request.chunked = true` for chunked
framing, and `request.trailers.push_back({"X-Checksum", "..."})` for safe trailer
metadata. The message still owns its complete buffered body.

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
auto response = co_await pace::http::get(io, endpoint, "/health", {}, closing);
// For a persistent client: http::client client(std::move(socket), {}, closing);
```

`graceful` waits for the peer or this budget and propagates cleanup errors. Zero
only attempts local notification; negative values are rejected. This is not a
request/read timeout. The default `no_wait` ignores the budget. Server final
response draining remains unchanged, including early request rejection.
See [close-policy semantics](../reference/http.md#final-response-close-policy).

## The server

```cpp
pace::task<pace::http::response> route(pace::http::request request) {
    if (request.target == "/health")
        co_return pace::http::response{.body = "ok\n"};
    co_return pace::http::response{.status = 404, .body = "Not Found\n"};
}
```

In `main`, construct a context/listener, spawn `pace::http::serve(listener, route)`
and call `io.run()`. Keep the listener alive until run returns; declare it after
the context so it is destroyed first. The complete
[`tutorial_http.cpp`](../../examples/tutorial_http.cpp) already does this:

```sh
xmake run tutorial_http serve
curl --http1.1 http://127.0.0.1:8080/health
curl --http1.1 -H "Expect:" --data-binary "hello" http://127.0.0.1:8080/echo
curl --http1.1 -I http://127.0.0.1:8080/health
```

Ctrl-C requests cooperative stop. The curl Expect override stays within this
API's scope: it does not implement the `100-continue` request handshake.

A handler owns its request, so its body and headers survive suspension. Returned
responses own their strings as well. Set `response.chunked = true` and fill
`response.trailers` to emit chunked output; leave the default for Content-Length.
HEAD and bodyless statuses are handled by the serializer. Set `response.close`
to end persistence. Shared state across handlers on different workers still
requires application synchronization.

## Add HTTPS

Build optional OpenSSL support with `--tls=y`. The shape of the HTTP call is the
same; add a TLS configuration and verified peer name:

```cpp
pace::tls::context trust; // OpenSSL default trust paths; verification is required.
trust.load_verify_file("my-ca.pem"); // Optional private CA or explicit CA bundle.
auto response = co_await pace::http::get(io, trust, {"127.0.0.1", 8443},
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
policy remain application responsibilities. Automatic DNS, redirects, retries
and decompression are absent. HTTP/2,
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
Next: [execution and worker selection](05-execution.md).
