# HTTP/3

URL get/post/fetch and async forms: [unified client contract](http-clients.md).

Short forms and owning helpers: [component conveniences](component-conveniences.md).

`<pace/http3.hpp>` is an optional application protocol module. It implements
RFC 9114 framing and RFC 9204 QPACK through nghttp3 on top of PACE QUIC; it does
not depend on the HTTP/1.1 or HTTP/2 implementations. Request, response, header,
and handler types live in the transport-independent `<pace/http_types.hpp>`.

Enable the `http3` build option. This requires the QUIC extension, OpenSSL 3.5+
with QUIC support, and nghttp3 (the local Linux validation uses 1.18.0). Both QUIC
contexts must advertise ALPN `{"h3"}`; the application entry points reject an
established connection that negotiated any other protocol. Clients verify the
certificate chain and the expected server hostname during QUIC connection setup.
The QUIC backend targets POSIX; see the [platform matrix](platform.md) for
functional CI coverage.

```cpp
pace::quic::context config;
config.load_verify_file("ca.pem");
config.set_alpn_protocols({"h3"});
pace::quic::connection transport(io, config);
co_await transport.connect({"127.0.0.1", 443}, "example.test");
std::vector<pace::http::request> requests(1);
requests[0].method = "GET";
requests[0].target = "/";
auto responses = co_await pace::http3::async_exchange(
    std::move(transport), "example.test", std::move(requests));
```

`async_exchange(connection, authority, requests, limits)` takes ownership of an
authenticated QUIC connection and sends a batch of requests on separate
bidirectional streams. Results preserve input order. The driver services reads,
writes, and stream credits together, including when the peer permits fewer
simultaneous streams than the batch contains. After all responses complete, it
closes the connection. Refused, reset, truncated, or malformed streams fail the
batch rather than returning an incomplete response.

`serve_connection(connection, handler, limits)` owns an already established
connection. `serve(listener, handler, limits)` accepts connections and isolates
individual peer failures. Configure the QUIC listener's server context with a
certificate chain, private key, and `{"h3"}` before starting it. Handler exceptions
produce a 500 response; I/O cancellation propagates. Complete requests run up to
`min(max_concurrent_handlers, max_concurrent_streams)` application handlers
concurrently (both default to 32). One connection driver owns nghttp3 and QUIC;
handler results enter a bounded queue and explicitly wake that driver. A handler
waiting for asynchronous I/O does not block protocol progress or other streams.
This is cooperative concurrency on one context, not CPU execution on extra
threads; synchronous work in a handler still blocks that context.

Queued requests and buffered responses remain bounded by the concurrent stream
limit. Set `max_concurrent_handlers = 1` to serialize application handlers while
continuing to drive the connection. Shared handler state must allow coroutine
interleaving. Responses may finish out of order; batch results preserve request
order. Connection exit discards queued requests, cancels active handlers locally,
and joins them before returning. Their PACE waits inherit the token; custom
awaiters must participate. Closing a listener cancels and joins accepted
connections before `serve` returns. See [structured scopes](structured-concurrency.md).

Each connection binds its own control stream and QPACK encoder/decoder streams.
SETTINGS, request and response HEADERS/DATA, trailers, and stream FIN travel over
real encrypted QUIC. Static-table and literal QPACK are supported; the advertised
dynamic decoder capacity is zero, avoiding blocked-header buffering. The driver
reports accepted writes and acknowledged bytes to nghttp3, keeps response bodies
alive until acknowledged, and uses QUIC event readiness instead of fixed sleeps.

Limits default to 16 KiB of decoded header/trailer bytes (including the RFC's
32-byte overhead per field), 100 fields, 1 MiB of body data per stream, 32
concurrent requests, 32 concurrent handlers, and 1000 requests per connection.
Both concurrency limits must be 1–10,000. Bounds apply to incoming
and outgoing messages. Bodies are buffered, so memory scales with the concurrent
stream count and body limit. Excessive peer streams are rejected. The API does
not provide streaming bodies, CONNECT tunnels, server push, HTTP Datagram,
WebTransport, or a reusable client connection pool. QUIC currently accepts
numeric IPv4/IPv6 endpoints; DNS resolution belongs to the separate DNS extension.

`examples/tutorial_http3.cpp` runs an authenticated localhost server and client.
`tests/http3_tests.cpp` covers multiplexed binary bodies beyond a QUIC send
buffer, a 130-request batch and stream cleanup, trailers, HEAD/205 semantics,
application failures, bounded handler concurrency, out-of-order completion,
requests arriving while another handler waits, late completion after disconnect,
ALPN enforcement, recovery after a failed peer handshake,
cancellation, incoming/outgoing limits, and independently encoded RFC 9114/QPACK
peers in both directions, including a mismatched content length. The full suite
also passes Linux AddressSanitizer, UndefinedBehaviorSanitizer, and leak checks.

Protocol integration references: [nghttp3 programmers' guide](https://nghttp2.org/nghttp3/programmers-guide.html),
[nghttp3 stream write API](https://nghttp2.org/nghttp3/nghttp3_conn_writev_stream.html).
