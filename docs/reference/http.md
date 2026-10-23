# `pace::http`: HTTP/1.1 and HTTPS

Include `<pace/http.hpp>`. PACE supplies an HTTP/1.0–1.1 server and client,
with Content-Length, chunked transfer, trailers, persistence and strict framing
validation. Buffered messages are the default; an opt-in server API adds
request-head admission, per-request body limits and bounded response streaming. TLS overloads use the same parser and require `--tls=y`
(`PACE_HAS_TLS`). HTTP/2 and WebSocket have separate APIs; this namespace neither
negotiates protocol upgrades nor implements CONNECT or HTTP/3.

## Start with the short API

```cpp
// Inside a coroutine running on io; endpoints accept numeric IPv4 or IPv6.
auto response = co_await pace::http::get(io, {"127.0.0.1", 8080}, "/health");

// HTTPS: configuration uses certificate verification by default.
pace::tls::context trust;
auto secure = co_await pace::http::get(io, trust, {"127.0.0.1", 8443},
                                     "localhost", "/health");
```

The TLS example needs a trusted certificate for `localhost`; configure a private
CA with `trust.load_verify_file(...)` before starting the request. The numeric
address determines where to connect; `peer_name` determines certificate identity,
SNI for DNS names, and the default Host. There is no insecure verification switch.
See the [HTTP/HTTPS tutorial](../tutorial/04-http.md) for a complete local example.

## Owned messages

```cpp
struct header_field { std::string name; std::string value; };
struct request {
    std::string method;
    std::string target;
    std::string version;
    std::vector<header_field> headers{};
    std::string body;
    bool keep_alive = true;
    std::vector<header_field> trailers{};
    bool chunked = false;
    std::string_view header(std::string_view name) const noexcept;
};
struct response {
    unsigned status = 200;
    std::string body;
    std::string content_type = "text/plain; charset=utf-8";
    std::vector<header_field> headers{};
    bool close = false;
    std::string version = "HTTP/1.1";
    std::vector<header_field> trailers{};
    bool chunked = false;
    std::string_view header(std::string_view name) const noexcept;
};
```

Strings and vectors own their contents. Method/target retain wire spelling and
are not URL-decoded or routed. Parsed versions are `HTTP/1.0` or `HTTP/1.1`.
A client's empty request version means HTTP/1.1. Bodies contain decoded framing
payload, including binary bytes; transfer chunk boundaries and chunk extensions
are not retained. These shared message types describe buffered transfer. The HTTP/1 server also
provides the opt-in admission and streaming API below; HTTP/2 and HTTP/3 keep
their existing message APIs.

`header(name)` returns the first ASCII case-insensitive match in `headers`, or an
empty view. Missing and empty are indistinguishable through that accessor alone.
The view borrows message storage; copy it before changing/destroying its owner.
Trailers are stored separately and are never silently merged into headers.

For received responses, `headers` retains actual wire fields, `content_type`
contains the first Content-Type value or empty, `chunked` identifies framing,
`version` records the peer version, and `close` states whether this exchange ends
reuse. For an outgoing server response, `version` is not a wire-format selector:
the server emits an HTTP/1.1 status line, including when handling HTTP/1.0 requests.

### Writing a message

A request's `keep_alive` controls its generated Connection field. Set `chunked`
for chunked transfer and optionally fill `trailers`. Otherwise PACE generates a
Content-Length for the owned body. A persistent `client` requires one valid Host
for HTTP/1.1; the `get`/`fetch` helpers fill a missing Host automatically.

A server response's `close` can end persistence. `content_type` selects the
Content-Type field; an empty string omits it. `chunked=true` emits a single data
chunk for a nonempty body followed by the terminal chunk and validated trailers.
This affects framing, not whether data is buffered. For HTTP/1.0 requests PACE
falls back to Content-Length; combining HTTP/1.0 and configured response trailers
is invalid and produces a closing 500.

Custom header names must be HTTP tokens; values cannot contain CR/LF, NUL or
other prohibited control characters. Content-Length, Transfer-Encoding,
Connection and Trailer are managed fields and cannot be supplied as custom
headers. Outgoing server Content-Type must use `content_type`, not custom headers.
The client also rejects Expect/Upgrade and CONNECT because this exchange API
cannot conduct those handshakes. Received responses may contain managed fields;
remove them before reusing that value as an outgoing server response.

Outgoing trailers require chunked framing. PACE validates names/values and emits
a Trailer declaration. Framing, routing, connection-control, authentication,
cookie and selected representation-control fields are forbidden as trailers.
The receiver additionally rejects fields named by Connection tokens. Ordinary
extension metadata such as `X-Checksum` is supported. Forbidden trailers are
rejected even when undeclared; safe undeclared trailers may be received.

### HEAD and bodyless status codes

The server's final status must be `[200, 599]`. It supplies standard reason phrases
for common codes, otherwise `Response`. There is no server informational-response
API. Invalid outgoing metadata becomes a closing 500 instead of injecting headers.

HEAD suppresses all body/chunk/trailer bytes while retaining applicable
representation framing metadata. 204 and 304 omit body and generated
Content-Length/Transfer-Encoding; 205 sends Content-Length 0 and no body.
A chunked handler can serve HEAD without a separate handler path. Trailers are
suppressed for bodyless replies. Error replies after recognition of HEAD likewise
contain no payload.

The client does not read bodies for HEAD, 204 or 304. Large representation lengths
on HEAD/304 do not trigger the body-byte cap. A 204 carrying Content-Length or
Transfer-Encoding is rejected. A 205 must decode to an empty body. Informational
100–199 responses are skipped up to a configured count; 101 is rejected because
upgrades require the separate protocol API. Ambiguous framing is rejected even
on a bodyless response.

Receiving `100 Continue` is distinct from an Expect handshake. The client sends
the entire request before reading responses and rejects a caller-supplied Expect
field. The server returns 417 for every Expect field, including `100-continue`.

## Receive limits

```cpp
struct limits {
    std::size_t max_header_bytes = 16 * 1024;
    std::size_t max_body_bytes = 1024 * 1024;
    std::size_t max_requests_per_connection = 1000;
    std::size_t max_header_fields = 100;
    std::size_t max_trailer_bytes = 8 * 1024;
    std::size_t max_trailer_fields = 32;
    std::size_t max_chunk_line_bytes = 1024;
    std::size_t max_chunks = 65536;
    std::size_t max_informational_responses = 8;
};
```

Header bytes include the start line and final CRLF delimiter. Trailer bytes
include field lines and the final empty CRLF. Chunk-line bytes include CRLF and
ignored extensions; chunk count includes the terminal zero chunk. The body cap
applies to decoded payload from Content-Length, chunks, or client EOF framing.
A declared Content-Length above the body cap is rejected before allocation/read.
Metadata fields and chunks are bounded independently to limit small-chunk attacks.

The request-count limit applies to server connections; the final permitted reply
closes. The informational-response cap applies to clients. Other byte/field limits
apply when receiving in either role. These limits do not cap locally constructed
outgoing messages, global memory, connection count or wall-clock lifetime. No
request/connect/handshake deadline is imposed by the original buffered API.
The opt-in server API below adds receive/write deadlines. Zero does not
mean unlimited: a zero allowance forbids that resource; a zero body allowance
still permits an empty body, and zero server requests closes without serving.

## Persistent clients

```cpp
template<class Transport> class basic_client {
public:
    explicit basic_client(Transport stream, limits bounds = {},
                          client_options options = {});
    basic_client(const basic_client&) = delete;
    basic_client& operator=(const basic_client&) = delete;
    task<response> send(request outgoing);
    Transport& transport() noexcept;
    bool is_open() const noexcept;
    void close() noexcept;
};
using client = basic_client<tcp::stream>;
#ifdef PACE_HAS_TLS
using secure_client = basic_client<tls::stream>;
#endif
```

Construct from an already-connected owned TCP stream or an already-handshaken TLS
stream. The provided instantiations are TCP and TLS; arbitrary transport template
instantiations are not provided. The client is noncopyable and nonmovable. Its
implicit destructor releases its transport; keep it alive until all sends finish.

`send` owns its request value, writes it, reads one final response, and retains
unconsumed parser bytes for the next exchange. Exactly **one exchange** can be
active at once; concurrent sends throw `logic_error` without disturbing the active
one. Client pipelined request submission/multiplexing is not supported. A server
can process multiple requests already buffered from a pipelined TCP input.

For Content-Length or chunked responses, complete framing permits reuse unless
either peer requests close. A response with no framing is read to EOF and cannot
be reused. The default final exchange completes after the validated HTTP message,
without waiting for the peer to close. Protocol, cancellation or transport errors
before message completion close the connection immediately.
`is_open` observes ownership/reuse eligibility, not a health probe. `close` is an
idempotent immediate close on the owning context; pending member operations must
still unwind before the object is destroyed. `transport()` is a borrowed reference
for idle configuration, not a way to interleave raw I/O with HTTP parsing.

Malformed/truncated/oversized peer responses throw `http::protocol_error`, derived
from `std::runtime_error`, and poison/close the connection. Its public constructor
is `explicit protocol_error(const std::string& message)`. Native/TLS errors and
context cancellation propagate as `std::system_error`. Local request validation
throws `invalid_argument` before writing, preserving a previously usable client;
closed clients and overlapping exchanges throw `logic_error`.

### Final-response close policy

```cpp
enum class client_close_policy { no_wait, graceful };
struct client_options {
    client_close_policy close_policy = client_close_policy::no_wait;
    std::chrono::milliseconds close_timeout{100};
};

// The same third argument is supported by secure_client.
pace::http::client client(std::move(connected_socket), {}, {
    .close_policy = pace::http::client_close_policy::graceful,
    .close_timeout = std::chrono::milliseconds(25),
});
```

`no_wait` attempts the local TCP FIN or TLS `close_notify` with a zero wait
budget, then releases the transport. It schedules no detached cleanup. It does
not wait for peer FIN, peer TLS alert, or `close_timeout`. Once a length-delimited,
chunked or bodyless response is complete, a transport error during this cleanup
does not replace that response. Context cancellation still propagates.

`graceful` explicitly waits for the peer or the configured bounded transport-close
budget; cleanup errors and cancellation propagate. A zero budget attempts only
the local close notification. Negative budgets and invalid policy values throw
`invalid_argument` at construction (or before a one-shot helper connects).
The timeout applies only to final cleanup, not to requests or response reads.

Neither policy shortens EOF-framed responses: these still require TCP EOF, or
an authenticated TLS `close_notify`. Truncated Content-Length, incomplete chunks,
invalid trailers and missing TLS EOF authentication still fail. HEAD and bodyless
status codes complete according to their validated HTTP framing. Persistent
exchanges never run final cleanup. See [RFC 9112 message framing](https://www.rfc-editor.org/rfc/rfc9112.html#section-6.3)
and [TLS connection closure](https://www.rfc-editor.org/rfc/rfc9112.html#section-9.8).
The server keeps its separate bounded drain after final responses, including
early rejection of an unread request body.

## One-shot clients

```cpp
task<response> fetch(io_context& context, tcp::endpoint endpoint,
                     request outgoing, limits bounds = {}, client_options options = {});
task<response> get(io_context& context, tcp::endpoint endpoint,
                   std::string target = "/", limits bounds = {}, client_options options = {});
#ifdef PACE_HAS_TLS
task<response> fetch(io_context& context, const tls::context& configuration,
                     tcp::endpoint endpoint, std::string peer_name,
                     request outgoing, limits bounds = {}, client_options options = {});
task<response> get(io_context& context, const tls::context& configuration,
                   tcp::endpoint endpoint, std::string peer_name,
                   std::string target = "/", limits bounds = {}, client_options options = {});
#endif
```

These helpers connect, request one closing exchange, and release the connection.
Their trailing `client_options` selects the same completion policy as `client`;
the default returns a fully framed response without awaiting peer closure.
`get` creates a GET; `fetch` uses the owned request you supply. Both override
`keep_alive=false`. If no Host exists, plain HTTP uses the endpoint address and
HTTPS uses `peer_name`; a nondefault port is appended (80 for HTTP, 443 for HTTPS).
Numeric IPv6 authorities are bracketed (for example `[::1]:8080`). A supplied
Host is retained and validated separately from TLS certificate identity.

Values are owned by the lazy task; the I/O context and referenced TLS configuration
must outlive its use. TLS streams retain configuration ownership once constructed,
but the lazy helper still initially borrows the configuration. All work runs on
the owning context. No DNS lookup, URL decomposition, redirect following,
automatic retry, proxy, cookie jar or content decompression occurs. A non-2xx HTTP
status is a normal response, not an exception.

The HTTP/1.1 TLS path accepts ALPN `http/1.1` or no negotiated ALPN and rejects
other protocols before HTTP bytes are exchanged. Configure a shared HTTP/1.1
TLS context with `set_alpn_protocols({"http/1.1"})` when desired; use the separate
HTTP/2 entry points when negotiating `h2`.

## Servers and handlers

```cpp
using handler = std::function<task<response>(request)>;
task<void> serve_connection(tcp::stream stream, handler handle, limits bounds = {});
task<void> serve(tcp::listener& listener, handler handle, limits bounds = {});
#ifdef PACE_HAS_TLS
task<void> serve_connection(tls::stream stream, handler handle, limits bounds = {});
task<void> serve(tcp::listener& listener, const tls::context& configuration,
                  handler handle, limits bounds = {});
#endif
```

Use a named coroutine handler taking an owned request value. Handler copies may
run concurrently on different contexts, so synchronize reachable shared state.
Within each connection requests and handlers are sequential. Pipelined bytes
remaining after one Content-Length/chunked message are retained for the next.

`serve_connection` owns the transport and handler. For TLS it expects handshake
completion already. It enables TCP_NODELAY and runs the HTTP loop. Per-connection
`system_error` failures, including cancellation and resets, are contained so
another client can continue. Unexpected non-system failures outside handler/error
conversion can still escape. There is no built-in error logging hook.

`serve` borrows the listener. It accepts and spawns independent connection tasks;
the TLS overload handshakes each accepted stream in its own task, so one pending
handshake does not serialize accepting other peers. Configuration/listener must
outlive the serving task. Closing the listener on its context cancels accepting;
accepted streams continue. Stop the context to cancel all its work. There is no
connection registry/join handle or automatic maximum-connections policy.

Expected accept cancellation/closed-listener conditions return normally; other
accept errors propagate. Handler exceptions become a closing 500, except
`system_error(operation_canceled)`, which exits as cancellation. Empty handlers
therefore produce 500. The final response uses the transport's bounded graceful
close (100 ms by default); neither response delivery nor peer acknowledgment is
guaranteed after transport failure.

## Admission before receiving a request body

The original `handler` and `limits` overloads remain supported. To opt in, use a
`server_handler`, which returns `task<server_response>`, and `server_options`:

```cpp
using server_response = std::variant<response, streaming_response>;
using server_handler = std::function<task<server_response>(request)>;

pace::task<pace::http::server_response> route(pace::http::request request) {
    co_return pace::http::response{.body = request.body};
}

pace::http::server_options options;
options.admit = [](const pace::http::request_head& head) {
    if (head.header("Authorization") != "Bearer example") {
        return pace::http::admission::reject(
            pace::http::response{.status = 401, .body = "unauthorized\n"});
    }
    return pace::http::admission::accept(
        head.target == "/mail" ? 16 * 1024 * 1024 : 64 * 1024);
};
options.request_timeout = std::chrono::seconds(15);
options.write_timeout = std::chrono::seconds(30);
// listener and its context remain alive while serving.
io.spawn(pace::http::serve(listener, route, options));
```

The same overloads exist for `serve_connection(tcp::stream, ...)`,
`serve_connection(tls::stream, ...)`, and `serve(listener, tls_configuration, ...)`.
The TLS path applies the same parser and policies after its handshake.

`request_head` exposes `method`, `target`, and `version` as string views,
`headers` as `span<const header_field>`, `content_length` as
`optional<size_t>`, and the `chunked` and `keep_alive` flags. Its `header(name)`
accessor matches ASCII names case-insensitively. These views are valid **only
inside the synchronous callback**; copy anything to retain. The request line,
headers, and framing have already passed the normal checks. Malformed or
ambiguous framing never reaches admission. No body or trailer collection takes
place first, although the final bounded socket read may include up to 8 KiB of
body or pipelined data alongside the head.

`admission::reject(response)` sends that response immediately and forces
`Connection: close`, regardless of `response::close`. PACE does not wait for the
advertised body. After writing, the existing bounded transport graceful-close
policy may discard unread input; it is not a prerequisite to sending the reply.
No final handler is invoked. A throwing admission callback produces a closing
500, except `operation_canceled`, which terminates the connection.

`admission::accept(n)` replaces `bounds.max_body_bytes` for **this request**.
It may choose a larger or smaller limit; it is not clamped to the connection's
default. `accept()` keeps that default and `accept(0)` permits only an empty
body. Content-Length exceeding the selected limit receives 413 before body
collection, and chunked input is checked cumulatively at each chunk-size line,
before reading that chunk. Metadata/trailer/chunk-count caps remain unchanged.
Admission is rerun for every keep-alive request, including already-pipelined
heads; no selected limit leaks into the next request. Accepted request bodies
are still buffered up to the selected cap before the final handler runs.

## Bounded response producers

```cpp
class body_source {
public:
    virtual ~body_source() = default;
    virtual std::size_t read(std::span<char> destination) = 0;
};
struct streaming_response {
    response head;
    std::uint64_t content_length = 0;
    std::unique_ptr<body_source> source;
};
```

Return either an ordinary `response` or a `streaming_response` from a
`server_handler`. Streaming uses the supplied Content-Length, works for
HTTP/1.0 and HTTP/1.1, and owns its source until completion or failure. Its
`head.body` and `head.trailers` must be empty and `head.chunked` false. Metadata
is validated before writing; invalid metadata produces a closing 500. Unknown
length streaming and streamed trailers are not supported by this interface.

PACE gives `read()` a writable span no larger than
`server_options::response_buffer_bytes` (64 KiB by default), or the remaining
Content-Length if smaller. Return a positive count no greater than that span's
size. The producer is pulled again only after the preceding `write_all`
finishes: the library never queues the full response in application memory.
Kernel and TLS transport buffers have their own bounded buffering. Exactly
Content-Length bytes are sent; the producer is not called once that length is
reached. A zero result before then, an oversized count, or a producer exception
closes the connection without trying to append a second response. Disconnect,
context cancellation, and write expiry stop future pulls and release the owned
source as the pending write unwinds. An OS send succeeding means local acceptance,
not acknowledgment by the peer; a disconnect is observed when the transport
reports it.

Calls run synchronously on the owning event-loop thread and must return
promptly. They are suitable for bounded generation and reads from an already
open local file. Blocking disk/network work or expensive computation should be
prepared by the application outside the event loop; this API cannot interrupt
an executing user callback. There is no asynchronous producer coroutine to
leave suspended after a disconnect. Different connections may call their own
producers concurrently on different worker contexts.

HEAD retains the applicable Content-Length but does not pull the source. 204,
205, and 304 also never pull it and follow the existing bodyless framing rules.
A source is optional when no bytes will be pulled (zero length or a bodyless
reply). Its destructor runs on all those paths too.

An open file can pin a selected snapshot instead of reopening its pathname for
each chunk. This example obtains its length from the same open handle:

```cpp
#include <fstream>

class file_body final : public pace::http::body_source {
public:
    explicit file_body(const std::string& path)
        : file_(path, std::ios::binary | std::ios::ate) {
        if (!file_) throw std::runtime_error("cannot open snapshot");
        const auto end = file_.tellg();
        if (end < 0) throw std::runtime_error("cannot size snapshot");
        size_ = static_cast<std::uint64_t>(end);
        file_.seekg(0);
        if (!file_) throw std::runtime_error("cannot seek snapshot");
    }
    std::uint64_t size() const noexcept { return size_; }
    std::size_t read(std::span<char> destination) override {
        file_.read(destination.data(), static_cast<std::streamsize>(destination.size()));
        if (file_.bad()) throw std::runtime_error("cannot read snapshot");
        return static_cast<std::size_t>(file_.gcount());
    }
private:
    std::ifstream file_;
    std::uint64_t size_ = 0;
};

pace::task<pace::http::server_response> backup(pace::http::request) {
    auto file = std::make_unique<file_body>("backup.tar");
    pace::http::streaming_response result;
    result.head.content_type = "application/octet-stream";
    result.content_length = file->size();
    result.source = std::move(file);
    co_return result;
}
```

The application must ensure the contents of that open file remain a stable
snapshot; retaining a handle does not prevent another writer modifying the same
file in place.

## Server deadlines

`server_options` contains `bounds`, `admit`, `request_timeout`, `write_timeout`,
and `response_buffer_bytes`. Both timeout values are `chrono::milliseconds`,
default zero (disabled), and reject negative values. A zero response buffer is
invalid. The timer handles are canceled when their phase ends, so completed
requests do not retain long-lived sleeping tasks or timers.

* The request budget starts before reading each head, including the wait for the
  next keep-alive request, and ends when the accepted body/trailers are complete
  or admission rejects. It includes synchronous admission. This is an absolute
  budget, not an inactivity timeout: trickling bytes does not restart it.
* The write budget starts when the response is ready, before encoding its head,
  and covers all response bytes and producer calls. Early rejection and parser
  error replies use the same write budget. It ends before graceful close, whose
  existing separate transport bound still applies.
* Expiry closes the connection and cancels pending transport operations. It does
  not promise an HTTP 408 or any complete response after a partial write. A
  rejected request does not continue running its request timer while its reply
  is sent.
* These budgets do not cover the final application handler, connection setup,
  or the TLS handshake. Application work and transport setup require their own
  lifetime/cancellation policy. Synchronous callbacks cannot be preempted;
  expiry is also checked after each admission/producer call, but a callback that
  never returns blocks the event loop.

## Strict framing and current boundaries


| Input | Server result / client behavior |
|---|---|
| Duplicate Content-Length, even identical; invalid numeric length | 400 / protocol error |
| Transfer-Encoding together with Content-Length | 400 / protocol error |
| Duplicate Transfer-Encoding fields | 400 / protocol error |
| Connection nominates Host, framing or Trailer fields | 400 / protocol error |
| Transfer coding other than a single `chunked` | 501 / protocol error |
| HTTP/1.0 with Transfer-Encoding | 400 / protocol error |
| Invalid chunk hex, extension grammar, terminator or trailer | 400 / protocol error |
| Body/chunk-count cap exceeded | 413 / protocol error |
| Header/trailer/chunk-line metadata cap exceeded | 431 / protocol error |
| HTTP/1.1 missing Host, duplicate/empty/invalid Host | 400 |
| Expect, including `100-continue` | 417; not emitted by client API |
| CONNECT, Upgrade request | 501; use appropriate separate protocol API |
| Unsupported request version | 505 |
| Incomplete message at EOF | 400 when possible / protocol error |

Chunk extensions support token/quoted-string values with checked escapes and are
ignored after validation. Safe trailers may be undeclared. Duplicate ordinary
headers are preserved rather than generically combined. Host checking is syntactic;
virtual hosting, authentication, authorization and origin policy belong to the
application. There is no implicit URL decoding, JSON parser or router.

HTTP/1.1 provides bounded buffered requests and either buffered or known-length
streamed server responses. It does not claim every HTTP feature: no content-coding
decompression, Expect handshake, CONNECT tunnel, server informational emission,
streamed request bodies, automatic redirect/retry, proxy or HTTP/3 within this
HTTP/1 module.
HTTP/2, [HTTP/3](http3.md) and WebSocket use their own APIs; explicit hostname
resolution is available through the optional [DNS extension](dns.md).
EOF-delimited TLS responses additionally require a clean TLS close_notify; TLS
truncation is a transport failure, not successful EOF.
