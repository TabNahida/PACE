# Public API reference

[Direct clients and HTTP name migration](direct-clients.md)

[Component conveniences (0.3.1)](component-conveniences.md) · [Protocol tuning](protocol-tuning.md) · [HTTP recipes](http-recipes.md) · [Build options](build-configuration.md)

[Application conveniences: app, async URL client, TCP, context](convenience.md)

Convenience APIs: [pace::run](run.md) · [HTTP routing and handler adapters](http-router.md) · [Guided tutorials](../tutorial/README.md)

[中文索引与关键约定](README.zh-CN.md)

This reference describes the public headers in `include/pace/`. Signatures use
C++20. A type in `pace::detail` is an implementation detail even if visible through
a public header; it has no compatibility guarantee.

| Header | Public symbols |
|---|---|
| [`task.hpp`](task.md) | `task<T = void>`, `task<void>`, their `promise_type`, `handle_type`, constructors, move assignment, destructor, `release`, `operator co_await`, nested `awaiter` and its protocol members |
| [`io_context.hpp`](io-context.md) | `io_context`, construction/destruction, `spawn`, `post`, `run`, `stop`, `stopped`, `current`, `cancel`, `wait_readable`, `wait_writable`, `sleep_for`, `call_at`, `call_after`, `timer_handle`, `yield`, `io_awaiter`, `timer_awaiter`, `yield_awaiter` |
| [`runtime.hpp`](runtime.md) | `runtime`, construction/destruction, `size`, `context`, `run`, `run_on_current_thread`, `stop` |
| [`blocking.hpp`](direct-clients.md) | `blocking_options`, shared direct-client deadline options |
| [`ip.hpp`](tcp.md) | Shared `address_family` for TCP and UDP endpoints |
| [`stream_io.hpp`](component-conveniences.md#byte-streams-and-tcp) | Owning `write`, bounded string/span `read_exact` for TCP/TLS/QUIC |
| [`tcp.hpp`](tcp.md) | `tcp::endpoint`, `tcp::listen_options`, `tcp::stream`, `tcp::listener` and all their members |
| [`udp.hpp`](udp.md) | `udp::endpoint`, `udp::datagram`, `udp::socket`, `send_to`, `receive_from` |
| [`dns.hpp`](dns.md) | Optional `dns::resolver`, A/AAAA results, deadlines and cancellation |
| [`quic.hpp`](quic.md) | Optional authenticated QUIC contexts, listeners, connections and multiplexed streams |
| [`http3.hpp`](http3.md) | Optional HTTP/3 batch `exchange`, `serve_connection`, `serve`, limits |
| `http_types.hpp` | Transport-independent `http::request`, `response`, `header_field`, `handler`, `protocol_error` |
| [`http.hpp`](http.md) | HTTP/1.1 types/limits, `request_head`, `admission`, `body_source`, `streaming_response`, `server_handler`, `server_options`, `serve_connection`, `serve`, `client`/`secure_client`, `basic_client`, direct `fetch`/`get`/`post`, asynchronous `async_fetch`/`async_get`/`async_post`, and HTTPS overloads |
| [`tls.hpp`](tls.md) | Optional verified TLS: `tls::context`, `tls::stream`, `trust_source`, `sni_policy`, SNI routing, PEM trust bundles, handshake, ALPN, record I/O and shutdown |
| [`http2.hpp`](http2.md) | Optional HTTP/2: `http2::limits`, batch `exchange`, `serve_connection`, `serve` |
| [`websocket.hpp`](websocket.md) | WebSocket handshake, messages, control frames and optional secure transport |
| [`platform.hpp`](platform.md) | `native_socket_type`, `invalid_socket` |
| [`net.hpp`](platform.md) | Compatibility aliases `tcp_stream`, `tcp_listener`, `ipv4_endpoint`, `listen_options` |
| [`pace.hpp`](platform.md) | Umbrella header; no additional types or functions |

## Rules shared by the APIs

For a runnable starting point, [choose a protocol example](../tutorial/README.md).
TLS needs `--tls=y`; DNS needs `--dns=y`; QUIC needs `--quic=y`.
HTTP/2 needs `--http2=y`; enable TLS as well for HTTP/2 over TLS.
HTTP/3 needs `--http3=y`, which also enables QUIC. See the
[four-layer module and header map](../architecture.md#four-independently-linkable-layers).

**Ownership.** A task is lazy and consumed once. Parameters passed by value become
part of its coroutine frame, but references, pointers, spans and string views do
not acquire ownership. The context must outlive its sockets, listeners and pending
operations. A socket object must remain at a stable address until its member
operations finish. Prefer named coroutine functions with owned value parameters.

**Execution.** One thread runs each context. `spawn`, `post` and `stop` are the
cross-thread entry points. Socket I/O and timer/yield waits execute on their owning
context. Construction before `run()` and cleanup after it returns are supported;
concurrent object access from an unrelated thread is not. Read-only accessors do
not make a concurrently modified socket safe to inspect.

Concurrency limits differ by protocol. TCP allows one read and one write at once;
the initial TLS stream allows **one asynchronous operation total**. Check the
specific protocol's contract before sharing a connection between coroutines.

**Errors.** Awaited tasks rethrow their exceptions. A root task or posted callback
with an unhandled exception stops its context; `run()` rethrows the first recorded
exception after draining. Runtime worker errors stop peer contexts and propagate
after joining. Allocation failures can propagate in addition to documented
argument, state and native I/O errors.

**Cancellation.** `stop()` is a permanent cooperative shutdown request, not a task
join or a forced interruption. Registered readiness waits and timers resume with
`std::system_error` whose code compares equal to
`std::errc::operation_canceled`. Catch that code where cancellation is expected.
An already-stopping context suppresses uncaught root cancellation while draining;
other errors remain failures. No per-task cancellation token exists.

**Progress.** A coroutine runs until it suspends or finishes. Blocking work and
unbounded CPU loops prevent that reactor from serving other work or finishing
shutdown. `run()` stays alive while idle until explicitly stopped. Contexts are
single-use and cannot be restarted.

The [tutorials](../tutorial/README.md) show these rules together. The linked signatures
describe the current implementation; they do not promise APIs from Asio or libuv.
