# WebSocket clients and servers

[简体中文](websocket.zh-CN.md) · [TLS](tls.md) · [Runnable example](../../examples/tutorial_websocket.cpp)

`<pace/websocket.hpp>` provides RFC 6455 WebSocket streams over TCP and, with
`--tls=y`, over verified TLS. The plain WebSocket implementation has no OpenSSL
dependency. It supports the HTTP/1.1 Upgrade handshake, text/binary messages,
fragmentation, subprotocol selection, ping/pong and the closing handshake.

```sh
xmake f -m release --examples=y --tests=y -y
xmake -y
xmake run tutorial_websocket          # Self-contained loopback exchange; exits.
xmake run tutorial_websocket serve 9092
xmake run tutorial_websocket client 9092  # From another terminal.
```

The server example accepts `ws://127.0.0.1:9092/echo`. A browser client can use
`new WebSocket("ws://127.0.0.1:9092/echo", "echo")`; the example binds loopback.
Ctrl-C stops server mode. Client/selftest mode has a five-second overall deadline.

## Client and server

```cpp
namespace ws = pace::websocket;

pace::task<void> client(pace::io_context& io) {
    pace::tcp::stream transport(io);
    co_await transport.connect({"127.0.0.1", 9092});
    ws::stream socket(std::move(transport), ws::role::client);
    co_await socket.handshake("localhost", "/echo", {"echo"});
    co_await socket.send_text("Hello from PACE");
    if (auto reply = co_await socket.receive()) {
        // reply->type is message_type::text or message_type::binary.
        std::cout << reply->data << '\n';
    }
    const bool acknowledged = co_await socket.close_gracefully();
    // False means the peer did not finish the closing handshake before the bound.
    (void)acknowledged;
}

pace::task<void> server(pace::tcp::stream transport) {
    ws::stream socket(std::move(transport), ws::role::server);
    const auto request = co_await socket.accept({"echo"}, [](const ws::upgrade_request& request) {
        return request.target == "/echo"; // Add your Origin/authentication policy here.
    });
    while (auto incoming = co_await socket.receive())
        co_await socket.send(std::move(*incoming));
}
```

The stream takes ownership of a connected TCP transport. The HTTP Upgrade is
performed exactly once. `handshake(host, target, protocols)` is for clients;
the host is an HTTP Host field, independent of the numeric TCP address. The
target is an origin-form path/query beginning with `/`, without a fragment.
`accept(supported_protocols, authorize)` is for servers. It returns the target,
host, Origin value and offered subprotocols. The optional synchronous callback
can reject the request with HTTP 403 before switching protocols. It must not
block the reactor. Authentication is application-owned; this initial callback
exposes those metadata fields, not arbitrary request headers.

The server chooses its first supported protocol offered by the client;
`selected_protocol()` returns the result or an empty string. No common subprotocol
is allowed by RFC 6455; enforce a required protocol in your application after
upgrade. Invalid upgrade fields fail with HTTP 400; a version other than 13 gets
HTTP 426 with the supported version. Clients validate the exact challenge,
Connection/Upgrade tokens, selected subprotocol and absence of unsolicited
extensions. Nonces and masking keys use the operating system's cryptographic
random generator on Linux, macOS, Windows and FreeBSD.

The WebSocket stream owns initial upgrade parsing, including any frame bytes
coalesced with the HTTP headers. Give it the TCP/TLS transport **before reading
HTTP bytes**. It is a separate endpoint from `http::serve`; there is no conversion
from an already-parsed HTTP request or HTTP-router upgrade callback in this API.

## Secure WebSocket (wss)

Enable `--tls=y`, configure trusted roots, and complete TLS before WebSocket:

```cpp
pace::tls::context config;
config.set_alpn_protocols({"http/1.1"});
pace::tls::stream transport(io, config);
co_await transport.connect({"192.0.2.1", 443}, "chat.example.com");
pace::websocket::secure_stream socket(std::move(transport), pace::websocket::role::client);
co_await socket.handshake("chat.example.com", "/chat");
```

The address is reserved for documentation; substitute your resolved server IP.
TLS verifies both the certificate chain and hostname. The server equivalent
wraps an accepted TCP stream in `tls::stream`, awaits `handshake()`, then moves it
into `secure_stream` with `role::server` and calls `accept()`. See [TLS](tls.md)
for certificate loading and the test-fixture example. ALPN must be empty or
`http/1.1`; negotiating `h2` is rejected because RFC 8441 Extended CONNECT is
not implemented. WebSocket does not add a URL parser or DNS resolver.

## Messages and controls

```cpp
task<void> send(message value, std::size_t fragment_bytes = 0);
task<void> send_text(std::string text, std::size_t fragment_bytes = 0);
task<void> send_binary(std::string bytes, std::size_t fragment_bytes = 0);
task<std::optional<message>> receive();
task<void> ping(std::string payload = {});
task<void> pong(std::string payload = {});
```

Outgoing data is owned by the operation throughout suspension, including NUL
bytes in binary data. A zero fragment size uses `max_frame_bytes`; a positive
size is capped at that bound. Sending validates UTF-8 for text and all configured
message/fragment limits. `receive()` assembles continuation frames into one
owned message and validates UTF-8 after assembly, including characters split
across frames. It automatically replies to ping and consumes pong, including
controls interleaved with a fragmented message. Control payloads are at most
125 bytes. There is no exposed pong callback or heartbeat scheduler.

**Only one asynchronous operation may be active per WebSocket stream**, including
send versus receive, handshake and close. This explicit initial contract also
applies to plain TCP WebSockets; independent reader and writer coroutines must
not share a stream. Overlap throws `logic_error` without changing the active
operation. It supports sequential request/response and echo sessions, not
unsolicited concurrent server pushes during a pending receive. Keep both the
stream and its reactor alive until all operations complete. Move only while
idle; run operations and `close()` on the owning reactor, or close while it is
not running. Application handlers should catch expected peer failures locally.

## Limits and protocol failures

Pass limits as the constructor's third argument:

| Limit | Default | Meaning |
| --- | ---: | --- |
| `max_header_bytes` | 16 KiB | HTTP upgrade head (allowed range 64 bytes–1 MiB) |
| `max_frame_bytes` | 64 KiB | Individual data-frame payload |
| `max_message_bytes` | 1 MiB | Reassembled application-message payload |
| `max_fragments` | 1024 | Data frames per application message |
| `max_control_frames` | 1024 | Ping/pong/close frames in one receive operation |

Data/fragment/control limits must be positive and frame size cannot exceed
message size. Header parsing also caps fields at 100. The implementation rejects
wrong mask direction, reserved bits/opcodes, unsupported extensions, nonminimal
length encodings, illegal continuations, fragmented/oversized controls, invalid
UTF-8, invalid close status codes and oversized messages. Protocol errors throw
`websocket::protocol_error`; `close_code()` is typically 1002 (protocol), 1007
(UTF-8), 1008 (control flood) or 1009 (size/fragment limit). After upgrade, it
attempts a corresponding close frame and closes the transport. Bare EOF without
a complete closing frame is a protocol failure, not successful message completion.

Transport errors retain `std::system_error`; reactor/stream cancellation uses
`operation_canceled`. Invalid local arguments use `invalid_argument`; invalid
state uses `logic_error`. Failures during an active operation close the
transport, except overlap rejection, which leaves the original operation active.

## Closing and scope

`send_close(code = 1000, reason = {})` initiates the closing handshake; then
call `receive()` to process its reply. `close_gracefully(code = 1000, reason = {},
timeout = 100ms)` sends close, receives until peer close or timeout, and always
closes the transport. It returns true for a completed peer close, false for a
timeout; non-timeout errors propagate. The timeout is cooperative and cannot
preempt blocking handlers. Its scoped timer is canceled and releases its callback
immediately when the exchange finishes, including early completion and errors.

`receive()` replies to a peer close automatically, gracefully closes the
underlying TCP/TLS transport, and returns `nullopt`. `peer_close_code()` and
`peer_close_reason()` preserve peer metadata; absent status yields an empty
optional. Reasons must be valid UTF-8 and at most 123 bytes. No application
messages may be sent after sending close. `close()` and destruction close
immediately without a WebSocket closing handshake; retain the object until
canceled members finish. After moving, only destruction, assignment, `close()`
and `is_open()` are supported on the source.

Compression (`permessage-deflate`), HTTP/2 Extended CONNECT, automatic reconnect,
streaming partial-message delivery and simultaneous read/write are not provided.
Tests cover plain and TLS roundtrips, adversarial framing/upgrade input, canceled
operations, timer lifetime, and independent Python-standard-library peers in
both roles. This is not an Autobahn conformance certification.

```sh
python3 tests/websocket_interop.py /path/to/tutorial_websocket
```
